/*
 * ext2.c — read-only ext2 filesystem driver for FiFi OS
 *
 * Supports:
 *   - 1KB, 2KB, 4KB block sizes
 *   - Direct blocks (i_block[0..11]) and single indirect (i_block[12])
 *   - Path resolution from root (inode 2)
 *   - File reads and directory listings
 *
 * I/O buffers are PMM-allocated pages (accessed via HHDM) because
 * virtio_blk_read() requires physical addresses it can DMA to, and
 * kernel .bss statics live at 0xFFFFFFFF80... which pmm_virt_to_phys
 * cannot translate.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ext2.h"
#include "virtio_blk.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"

/* ── on-disk structures ───────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   /* 1 for 1KB blocks, 0 for ≥4KB */
    uint32_t s_log_block_size;     /* block_size = 1024 << this */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;          /* 0=old, 1=dynamic (has s_inode_size) */
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;         /* valid if s_rev_level >= 1 */
} ext2_sb_t;

#define EXT2_MAGIC 0xEF53u

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_bgd_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;          /* 128 bytes; s_inode_size may be larger (ignored extra) */

#define EXT2_IMODE_DIR  0x4000u
#define EXT2_IMODE_FILE 0x8000u

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];  /* NOT null-terminated; use name_len */
} ext2_dirent_t;

/* ── driver state ─────────────────────────────────────────────────────────── */

static struct {
    bool      ready;
    uint32_t  block_size;
    uint32_t  inodes_per_group;
    uint32_t  blocks_per_group;
    uint32_t  inode_size;       /* stride in inode table (≥128) */
    uint32_t  first_data_block; /* s_first_data_block */
    uint32_t  num_groups;

    ext2_bgd_t *bgdt;           /* kmalloc'd block group descriptor table */

    /* PMM-allocated I/O pages (2 × 4096 bytes) */
    uint64_t io_phys;           /* scratch page for block reads */
    uint64_t ind_phys;          /* scratch page for indirect block pointers */
} g;

/* ── private helpers ──────────────────────────────────────────────────────── */

static void e2_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}


/* Read one filesystem block into the io_phys page, then copy to dst. */
static bool e2_read_block(uint32_t blk, void *dst) {
    if (blk == 0) return false;
    uint32_t spb    = g.block_size / 512;
    uint64_t sector = (uint64_t)blk * spb;
    uint8_t *tmp    = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!virtio_blk_read(sector, tmp, spb)) return false;
    if (dst && dst != tmp) e2_memcpy(dst, tmp, g.block_size);
    return true;
}

/* Read block directly into the io_phys HHDM page, return pointer to it. */
static uint8_t *e2_read_block_inplace(uint32_t blk) {
    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!e2_read_block(blk, buf)) return (uint8_t*)0;
    return buf;
}

/* Read an inode. Returns false on error. */
static bool e2_read_inode(uint32_t ino, ext2_inode_t *out) {
    if (ino == 0 || !g.ready) return false;

    uint32_t group = (ino - 1) / g.inodes_per_group;
    uint32_t index = (ino - 1) % g.inodes_per_group;

    if (group >= g.num_groups) return false;

    uint32_t table_block = g.bgdt[group].bg_inode_table;
    uint32_t byte_off    = index * g.inode_size;
    uint32_t blk_off     = byte_off / g.block_size;
    uint32_t in_blk      = byte_off % g.block_size;

    uint8_t *buf = e2_read_block_inplace(table_block + blk_off);
    if (!buf) return false;

    e2_memcpy(out, buf + in_blk, sizeof(ext2_inode_t));
    return true;
}

/* Name compare: entry name (not NUL-terminated, length in name_len) vs path component. */
static bool e2_name_eq(const ext2_dirent_t *e, const char *name) {
    uint8_t len = e->name_len;
    for (uint8_t i = 0; i < len; i++) {
        if (name[i] == '\0' || name[i] != e->name[i]) return false;
    }
    return name[len] == '\0';
}

/* Look up a name in a directory inode. Returns inode number, 0 if not found. */
static uint32_t e2_lookup(const ext2_inode_t *dir, const char *name) {
    uint8_t *blk_buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);

    /* Direct blocks */
    for (int b = 0; b < 12; b++) {
        if (dir->i_block[b] == 0) break;

        uint32_t spb    = g.block_size / 512;
        uint64_t sector = (uint64_t)dir->i_block[b] * spb;
        if (!virtio_blk_read(sector, blk_buf, spb)) continue;

        uint8_t *p   = blk_buf;
        uint8_t *end = blk_buf + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0 && e2_name_eq(de, name))
                return de->inode;
            p += de->rec_len;
        }
    }

    /* Single indirect */
    if (dir->i_block[12] != 0) {
        uint32_t *ptrs = (uint32_t*)pmm_phys_to_virt(g.ind_phys);
        uint32_t  spb  = g.block_size / 512;
        if (virtio_blk_read((uint64_t)dir->i_block[12] * spb, ptrs, spb)) {
            uint32_t nptrs = g.block_size / 4;
            for (uint32_t i = 0; i < nptrs; i++) {
                if (ptrs[i] == 0) break;
                uint64_t sector = (uint64_t)ptrs[i] * spb;
                if (!virtio_blk_read(sector, blk_buf, spb)) continue;
                uint8_t *p   = blk_buf;
                uint8_t *end = blk_buf + g.block_size;
                while (p < end) {
                    ext2_dirent_t *de = (ext2_dirent_t*)p;
                    if (de->rec_len == 0) break;
                    if (de->inode != 0 && e2_name_eq(de, name))
                        return de->inode;
                    p += de->rec_len;
                }
            }
        }
    }

    return 0;
}

/* Resolve an absolute path from root. Returns inode number, 0 if not found. */
static uint32_t e2_resolve(const char *path) {
    if (!path || path[0] != '/') return 0;
    path++;

    uint32_t cur = 2; /* root inode */
    if (*path == '\0') return cur;

    while (*path) {
        /* Extract next component */
        char comp[256];
        int  clen = 0;
        while (*path && *path != '/' && clen < 255)
            comp[clen++] = *path++;
        comp[clen] = '\0';
        if (*path == '/') path++;
        if (clen == 0) continue;

        ext2_inode_t inode;
        if (!e2_read_inode(cur, &inode)) return 0;
        if ((inode.i_mode & 0xF000u) != EXT2_IMODE_DIR) return 0;

        cur = e2_lookup(&inode, comp);
        if (cur == 0) return 0;
    }
    return cur;
}

/* ── public API ───────────────────────────────────────────────────────────── */

bool ext2_init(void) {
    if (!virtio_blk_present()) {
        kprintf("[ext2] no block device\n");
        return false;
    }

    /* Allocate PMM scratch pages before any I/O */
    g.io_phys  = pmm_alloc_page();
    g.ind_phys = pmm_alloc_page();
    if (!g.io_phys || !g.ind_phys) {
        kprintf("[ext2] PMM alloc failed\n");
        return false;
    }

    /* Read superblock: 2 sectors at sector 2 (byte offset 1024) */
    uint8_t *tmp = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!virtio_blk_read(2, tmp, 2)) {
        kprintf("[ext2] superblock read failed\n");
        return false;
    }
    ext2_sb_t sb;
    e2_memcpy(&sb, tmp, sizeof(ext2_sb_t));

    if (sb.s_magic != EXT2_MAGIC) {
        kprintf("[ext2] bad magic: 0x%x\n", (unsigned)sb.s_magic);
        return false;
    }

    g.block_size       = 1024u << sb.s_log_block_size;
    g.blocks_per_group = sb.s_blocks_per_group;
    g.inodes_per_group = sb.s_inodes_per_group;
    g.first_data_block = sb.s_first_data_block;
    g.inode_size       = (sb.s_rev_level >= 1u) ? (uint32_t)sb.s_inode_size : 128u;

    if (g.block_size > 4096u) {
        kprintf("[ext2] unsupported block size %u\n", (unsigned)g.block_size);
        return false;
    }
    if (g.inode_size < sizeof(ext2_inode_t)) {
        kprintf("[ext2] inode_size %u < struct size\n", (unsigned)g.inode_size);
        return false;
    }

    g.num_groups = (sb.s_blocks_count + sb.s_blocks_per_group - 1)
                   / sb.s_blocks_per_group;

    kprintf("[ext2] block_size=%u inode_size=%u groups=%u\n",
            (unsigned)g.block_size, (unsigned)g.inode_size, (unsigned)g.num_groups);

    /* Read block group descriptor table (block after superblock's block) */
    uint32_t bgdt_block = g.first_data_block + 1;
    uint32_t bgdt_bytes = g.num_groups * (uint32_t)sizeof(ext2_bgd_t);

    g.bgdt = (ext2_bgd_t*)kmalloc(bgdt_bytes);
    if (!g.bgdt) {
        kprintf("[ext2] bgdt alloc failed\n");
        return false;
    }

    /* Read enough blocks to cover the BGD table */
    uint32_t bgdt_blocks = (bgdt_bytes + g.block_size - 1) / g.block_size;
    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        uint32_t spb    = g.block_size / 512;
        uint64_t sector = (uint64_t)(bgdt_block + i) * spb;
        if (!virtio_blk_read(sector, tmp, spb)) {
            kprintf("[ext2] bgdt block %u read failed\n", (unsigned)i);
            return false;
        }
        uint32_t chunk = g.block_size;
        uint32_t off   = i * g.block_size;
        if (off + chunk > bgdt_bytes) chunk = bgdt_bytes - off;
        e2_memcpy((uint8_t*)g.bgdt + off, tmp, chunk);
    }

    g.ready = true;
    kprintf("[ext2] ready — root inode 2\n");
    return true;
}

bool ext2_present(void) { return g.ready; }

int ext2_read_file(const char *path, void *buf, uint32_t max) {
    if (!g.ready) return -1;

    uint32_t ino = e2_resolve(path);
    if (!ino) return -1;

    ext2_inode_t inode;
    if (!e2_read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & 0xF000u) != EXT2_IMODE_FILE) return -1;

    uint32_t file_size = inode.i_size;
    uint32_t to_read   = (file_size < max) ? file_size : max;
    uint32_t done      = 0;
    uint8_t *out       = (uint8_t*)buf;
    uint32_t spb       = g.block_size / 512;

    uint8_t *io = (uint8_t*)pmm_phys_to_virt(g.io_phys);

    /* Direct blocks */
    for (int b = 0; b < 12 && done < to_read; b++) {
        if (inode.i_block[b] == 0) break;
        uint64_t sector = (uint64_t)inode.i_block[b] * spb;
        if (!virtio_blk_read(sector, io, spb)) return -1;
        uint32_t chunk = g.block_size;
        if (done + chunk > to_read) chunk = to_read - done;
        e2_memcpy(out + done, io, chunk);
        done += chunk;
    }

    /* Single indirect */
    if (done < to_read && inode.i_block[12] != 0) {
        uint32_t *ptrs = (uint32_t*)pmm_phys_to_virt(g.ind_phys);
        uint64_t  isec = (uint64_t)inode.i_block[12] * spb;
        if (!virtio_blk_read(isec, ptrs, spb)) return -1;

        uint32_t nptrs = g.block_size / 4;
        for (uint32_t i = 0; i < nptrs && done < to_read; i++) {
            if (ptrs[i] == 0) break;
            uint64_t sector = (uint64_t)ptrs[i] * spb;
            if (!virtio_blk_read(sector, io, spb)) return -1;
            uint32_t chunk = g.block_size;
            if (done + chunk > to_read) chunk = to_read - done;
            e2_memcpy(out + done, io, chunk);
            done += chunk;
        }
    }

    return (int)done;
}

int ext2_file_size(const char *path) {
    if (!g.ready) return -1;
    uint32_t ino = e2_resolve(path);
    if (!ino) return -1;
    ext2_inode_t inode;
    if (!e2_read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & 0xF000u) != EXT2_IMODE_FILE) return -1;
    return (int)inode.i_size;
}

void ext2_ls(const char *path) {
    if (!g.ready) { kprintf("[ext2] not ready\n"); return; }

    uint32_t ino = e2_resolve(path);
    if (!ino) { kprintf("[ext2] ls: not found: %s\n", path); return; }

    ext2_inode_t inode;
    if (!e2_read_inode(ino, &inode)) return;
    if ((inode.i_mode & 0xF000u) != EXT2_IMODE_DIR) {
        kprintf("[ext2] ls: not a directory: %s\n", path);
        return;
    }

    uint8_t  *blk = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    uint32_t  spb = g.block_size / 512;

    for (int b = 0; b < 12; b++) {
        if (inode.i_block[b] == 0) break;
        uint64_t sector = (uint64_t)inode.i_block[b] * spb;
        if (!virtio_blk_read(sector, blk, spb)) continue;

        uint8_t *p   = blk;
        uint8_t *end = blk + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0) {
                char type = (de->file_type == 2) ? 'd' : 'f';
                kprintf("  [%c] ", type);
                for (uint8_t i = 0; i < de->name_len; i++)
                    kprintf("%c", de->name[i]);
                kprintf("\n");
            }
            p += de->rec_len;
        }
    }
}

size_t ext2_ls_buf(char *buf, size_t cap) {
    if (!buf || cap == 0 || !g.ready) return 0;

    ext2_inode_t root;
    if (!e2_read_inode(2, &root)) return 0;
    if ((root.i_mode & 0xF000u) != EXT2_IMODE_DIR) return 0;

    uint8_t  *blk = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    uint32_t  spb = g.block_size / 512;
    size_t pos = 0;

    for (int b = 0; b < 12; b++) {
        if (root.i_block[b] == 0) break;
        uint64_t sector = (uint64_t)root.i_block[b] * spb;
        if (!virtio_blk_read(sector, blk, spb)) continue;

        uint8_t *p   = blk;
        uint8_t *end = blk + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len > 0) {
                /* skip "." and ".." */
                if (!(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                    for (uint8_t i = 0; i < de->name_len && pos + 1 < cap; i++)
                        buf[pos++] = de->name[i];
                    if (pos + 1 < cap) buf[pos++] = '\n';
                }
            }
            p += de->rec_len;
        }
    }

    if (pos < cap) buf[pos] = '\0';
    return pos;
}
