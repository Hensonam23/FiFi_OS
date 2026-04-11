/*
 * ext2.c — ext2 filesystem driver for FiFi OS (read + write)
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
#include "pit.h"

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

/* List entries of the directory at path into buf ("name\n" format). */
size_t ext2_ls_buf_at(const char *path, char *buf, size_t cap) {
    if (!buf || cap == 0 || !g.ready) return 0;

    uint32_t ino = path ? e2_resolve(path) : 2u;
    if (!ino) return 0;

    ext2_inode_t dir;
    if (!e2_read_inode(ino, &dir)) return 0;
    if ((dir.i_mode & 0xF000u) != EXT2_IMODE_DIR) return 0;

    uint8_t  *blk = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    uint32_t  spb = g.block_size / 512;
    size_t pos = 0;

    for (int b = 0; b < 12; b++) {
        if (dir.i_block[b] == 0) break;
        uint64_t sector = (uint64_t)dir.i_block[b] * spb;
        if (!virtio_blk_read(sector, blk, spb)) continue;

        uint8_t *p   = blk;
        uint8_t *end = blk + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len > 0) {
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

/* ── write support ────────────────────────────────────────────────────────── */

static void e2_memset(void *dst, uint8_t v, size_t n) {
    volatile uint8_t *d = (volatile uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = v;
}

/* Write one filesystem block from src (any kernel virtual address). */
static bool e2_write_block(uint32_t blk, const void *src) {
    if (blk == 0) return false;
    uint32_t spb    = g.block_size / 512;
    uint64_t sector = (uint64_t)blk * spb;
    return virtio_blk_write(sector, src, (size_t)spb);
}

/* Write in-memory BGD for group back to disk. */
static bool e2_flush_bgd(uint32_t gidx) {
    uint32_t bgdt_block = g.first_data_block + 1;
    uint32_t off        = gidx * (uint32_t)sizeof(ext2_bgd_t);
    uint32_t blk_off    = off / g.block_size;
    uint32_t in_blk     = off % g.block_size;

    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!e2_read_block(bgdt_block + blk_off, buf)) return false;
    e2_memcpy(buf + in_blk, &g.bgdt[gidx], sizeof(ext2_bgd_t));
    return e2_write_block(bgdt_block + blk_off, buf);
}

/* Write inode struct back to inode table on disk. */
static bool e2_write_inode(uint32_t ino, const ext2_inode_t *inode) {
    if (ino == 0 || !g.ready) return false;
    uint32_t group = (ino - 1) / g.inodes_per_group;
    uint32_t index = (ino - 1) % g.inodes_per_group;
    if (group >= g.num_groups) return false;

    uint32_t table_block = g.bgdt[group].bg_inode_table;
    uint32_t byte_off    = index * g.inode_size;
    uint32_t blk_off     = byte_off / g.block_size;
    uint32_t in_blk      = byte_off % g.block_size;

    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!e2_read_block(table_block + blk_off, buf)) return false;
    e2_memcpy(buf + in_blk, inode, sizeof(ext2_inode_t));
    return e2_write_block(table_block + blk_off, buf);
}

/* Mark a data block as free in its group's bitmap. */
static void e2_free_block(uint32_t blk) {
    if (blk < g.first_data_block) return;
    uint32_t adj  = blk - g.first_data_block;
    uint32_t gidx = adj / g.blocks_per_group;
    uint32_t bit  = adj % g.blocks_per_group;
    if (gidx >= g.num_groups) return;

    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    if (!e2_read_block(g.bgdt[gidx].bg_block_bitmap, buf)) return;
    buf[bit / 8] &= ~(1u << (bit % 8));
    if (!e2_write_block(g.bgdt[gidx].bg_block_bitmap, buf)) return;
    g.bgdt[gidx].bg_free_blocks_count++;
    (void)e2_flush_bgd(gidx);
}

/* Allocate a free data block. Returns absolute block number, 0 on failure. */
static uint32_t e2_alloc_block(void) {
    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    for (uint32_t gidx = 0; gidx < g.num_groups; gidx++) {
        if (g.bgdt[gidx].bg_free_blocks_count == 0) continue;
        uint32_t bmap = g.bgdt[gidx].bg_block_bitmap;
        if (!e2_read_block(bmap, buf)) continue;
        uint32_t nbits = g.blocks_per_group;
        for (uint32_t bit = 0; bit < nbits; bit++) {
            if (!(buf[bit / 8] & (1u << (bit % 8)))) {
                buf[bit / 8] |= (1u << (bit % 8));
                if (!e2_write_block(bmap, buf)) return 0;
                g.bgdt[gidx].bg_free_blocks_count--;
                (void)e2_flush_bgd(gidx);
                return g.first_data_block + gidx * g.blocks_per_group + bit;
            }
        }
    }
    return 0;
}

/* Allocate a free inode. Returns 1-based inode number, 0 on failure. */
static uint32_t e2_alloc_inode(void) {
    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    for (uint32_t gidx = 0; gidx < g.num_groups; gidx++) {
        if (g.bgdt[gidx].bg_free_inodes_count == 0) continue;
        uint32_t imap = g.bgdt[gidx].bg_inode_bitmap;
        if (!e2_read_block(imap, buf)) continue;
        uint32_t nbits = g.inodes_per_group;
        for (uint32_t bit = 0; bit < nbits; bit++) {
            if (!(buf[bit / 8] & (1u << (bit % 8)))) {
                buf[bit / 8] |= (1u << (bit % 8));
                if (!e2_write_block(imap, buf)) return 0;
                g.bgdt[gidx].bg_free_inodes_count--;
                (void)e2_flush_bgd(gidx);
                return gidx * g.inodes_per_group + bit + 1;
            }
        }
    }
    return 0;
}

/*
 * Add an entry to a directory inode.
 * dir_ino: inode of the directory to modify.
 * name / name_len: the new file's name.
 * new_ino: inode number to link.
 * ftype: EXT2_FT_* (1 = regular file, 2 = directory).
 *
 * Uses g.io_phys as scratch but saves dir_inode on the stack first, so
 * e2_alloc_block (which also uses io_phys) is safe to call after scanning.
 */
static bool e2_add_dirent(uint32_t dir_ino, const char *name, uint32_t name_len,
                           uint32_t new_ino, uint8_t ftype) {
    ext2_inode_t dir_inode;
    if (!e2_read_inode(dir_ino, &dir_inode)) return false;

    uint32_t needed = ((8u + name_len) + 3u) & ~3u;
    uint8_t *buf    = (uint8_t*)pmm_phys_to_virt(g.io_phys);

    /* Scan existing directory blocks for a free slot. */
    for (int b = 0; b < 12; b++) {
        uint32_t dblk = dir_inode.i_block[b];
        if (dblk == 0) {
            /* No space found — allocate a new directory block.
             * e2_alloc_block reuses io_phys; we are done scanning so this is safe. */
            uint32_t new_blk = e2_alloc_block();
            if (!new_blk) return false;
            e2_memset(buf, 0, g.block_size);
            ext2_dirent_t *de = (ext2_dirent_t*)buf;
            de->inode     = new_ino;
            de->rec_len   = (uint16_t)g.block_size;
            de->name_len  = (uint8_t)name_len;
            de->file_type = ftype;
            e2_memcpy(de->name, name, name_len);
            if (!e2_write_block(new_blk, buf)) return false;
            dir_inode.i_block[b]  = new_blk;
            dir_inode.i_blocks   += g.block_size / 512;
            dir_inode.i_size     += g.block_size;
            return e2_write_inode(dir_ino, &dir_inode);
        }

        if (!e2_read_block(dblk, buf)) continue;

        uint8_t *p   = buf;
        uint8_t *end = buf + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;

            /* Minimum space actually occupied by this entry */
            uint32_t actual = (de->inode != 0)
                              ? ((8u + (uint32_t)de->name_len + 3u) & ~3u) : 0u;
            uint32_t slack  = de->rec_len - (de->inode != 0 ? actual : 0u);

            if (slack >= needed) {
                if (de->inode != 0) {
                    /* Split: shrink this entry, put new one in the gap. */
                    uint16_t old_len = de->rec_len;
                    de->rec_len = (uint16_t)actual;
                    ext2_dirent_t *ne = (ext2_dirent_t*)(p + actual);
                    ne->inode     = new_ino;
                    ne->rec_len   = (uint16_t)(old_len - actual);
                    ne->name_len  = (uint8_t)name_len;
                    ne->file_type = ftype;
                    e2_memcpy(ne->name, name, name_len);
                } else {
                    /* Reuse a deleted-entry slot. */
                    de->inode     = new_ino;
                    de->name_len  = (uint8_t)name_len;
                    de->file_type = ftype;
                    e2_memcpy(de->name, name, name_len);
                }
                return e2_write_block(dblk, buf);
            }
            p += de->rec_len;
        }
    }
    return false; /* all 12 direct directory blocks full */
}

/*
 * e2_split_path — split "/a/b/c" into parent="/a/b" and name="c".
 * For root-level paths like "/foo", parent="/" and name="foo".
 * Returns name_len (> 0) on success, 0 on error.
 */
static uint32_t e2_split_path(const char *path,
                               char *parent_out, uint32_t parent_cap,
                               const char **name_out) {
    if (!path || path[0] != '/') return 0;

    /* Find last '/' */
    int last = 0;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last = i;

    if (last == 0) {
        if (parent_cap < 2) return 0;
        parent_out[0] = '/';
        parent_out[1] = '\0';
    } else {
        if ((uint32_t)last + 1 > parent_cap) return 0;
        for (int i = 0; i < last; i++) parent_out[i] = path[i];
        parent_out[last] = '\0';
    }

    *name_out = path + last + 1;
    uint32_t name_len = 0;
    while ((*name_out)[name_len] && name_len < 255) name_len++;
    return name_len;
}

/*
 * ext2_write_file — create or overwrite a file on the ext2 disk.
 *
 * path may contain subdirectory components (e.g. "/a/b/c").
 * The parent directory must already exist.
 * Handles files up to (12 + block_size/4) × block_size bytes.
 * Overwrites: frees old data blocks, writes new data.
 * Returns 0 on success, -1 on error.
 */
int ext2_write_file(const char *path, const void *data, uint32_t size) {
    if (!g.ready || !path || path[0] != '/') return -1;

    char parent[256];
    const char *name;
    uint32_t name_len = e2_split_path(path, parent, sizeof(parent), &name);
    if (name_len == 0) return -1;

    uint32_t parent_ino = e2_resolve(parent);
    if (parent_ino == 0) return -1;

    /* Cap at 12 direct blocks + single indirect */
    uint32_t max_direct   = 12u * g.block_size;
    uint32_t max_indirect = (g.block_size / 4u) * g.block_size;
    uint32_t max_size     = max_direct + max_indirect;
    if (size > max_size) size = max_size;

    /* Does the file already exist? */
    uint32_t     ino    = e2_resolve(path);
    ext2_inode_t inode;
    bool         is_new = (ino == 0);

    if (!is_new) {
        if (!e2_read_inode(ino, &inode)) return -1;
        if ((inode.i_mode & 0xF000u) != EXT2_IMODE_FILE) return -1;
        /* Free existing direct data blocks */
        for (int b = 0; b < 12; b++) {
            if (inode.i_block[b] == 0) break;
            e2_free_block(inode.i_block[b]);
            inode.i_block[b] = 0;
        }
        /* Free existing indirect block and its children */
        if (inode.i_block[12] != 0) {
            uint8_t *ibuf = (uint8_t*)pmm_phys_to_virt(g.ind_phys);
            if (e2_read_block(inode.i_block[12], ibuf)) {
                uint32_t *ptrs = (uint32_t*)ibuf;
                uint32_t nptrs = g.block_size / 4;
                for (uint32_t i = 0; i < nptrs; i++) {
                    if (ptrs[i] == 0) break;
                    e2_free_block(ptrs[i]);
                }
            }
            e2_free_block(inode.i_block[12]);
            inode.i_block[12] = 0;
        }
        inode.i_blocks = 0;
        inode.i_size   = 0;
    } else {
        ino = e2_alloc_inode();
        if (ino == 0) return -1;
        e2_memset(&inode, 0, sizeof(inode));
        inode.i_mode        = EXT2_IMODE_FILE | 0644u;
        inode.i_links_count = 1;
    }

    /* Write direct data blocks */
    const uint8_t *src       = (const uint8_t*)data;
    uint32_t       remaining = size;
    uint32_t       offset    = 0;
    uint8_t       *buf       = (uint8_t*)pmm_phys_to_virt(g.io_phys);

    for (int b = 0; b < 12 && remaining > 0; b++) {
        uint32_t blk = e2_alloc_block();
        if (!blk) goto fail;

        uint32_t chunk = (remaining < g.block_size) ? remaining : g.block_size;
        e2_memset(buf, 0, g.block_size);
        e2_memcpy(buf, src + offset, chunk);
        if (!e2_write_block(blk, buf)) goto fail;

        inode.i_block[b]  = blk;
        inode.i_blocks   += g.block_size / 512;
        offset    += chunk;
        remaining -= chunk;
    }

    /* Write singly-indirect block if more data remains */
    if (remaining > 0) {
        uint32_t *ptrs  = (uint32_t*)pmm_phys_to_virt(g.ind_phys);
        uint32_t nptrs  = g.block_size / 4;
        e2_memset(ptrs, 0, g.block_size);

        uint32_t ind_blk = e2_alloc_block();
        if (!ind_blk) goto fail;

        for (uint32_t i = 0; i < nptrs && remaining > 0; i++) {
            uint32_t blk = e2_alloc_block();
            if (!blk) goto fail;

            uint32_t chunk = (remaining < g.block_size) ? remaining : g.block_size;
            e2_memset(buf, 0, g.block_size);
            e2_memcpy(buf, src + offset, chunk);
            if (!e2_write_block(blk, buf)) goto fail;

            ptrs[i]           = blk;
            inode.i_blocks   += g.block_size / 512;
            offset    += chunk;
            remaining -= chunk;
        }

        /* Write the pointer block (uses ind_phys, separate from io_phys) */
        if (!e2_write_block(ind_blk, (uint8_t*)ptrs)) goto fail;
        inode.i_block[12]  = ind_blk;
        inode.i_blocks    += g.block_size / 512;
    }

    inode.i_size = size;
    {
        uint32_t now = (uint32_t)(pit_get_ticks() / 100);
        inode.i_mtime = now;
        inode.i_atime = now;
        if (is_new) inode.i_ctime = now;
    }

    if (!e2_write_inode(ino, &inode)) goto fail;

    /* For new files, add the directory entry last (so a partial write
     * doesn't leave a dangling directory entry). */
    if (is_new) {
        if (!e2_add_dirent(parent_ino, name, name_len, ino, 1 /*EXT2_FT_REG_FILE*/))
            goto fail;
    }

    return 0;

fail:
    return -1;
}

/* ── delete / mkdir shared helpers ───────────────────────────────────────── */

/* Free an inode: clear its bitmap bit and increment group free count. */
static void e2_free_inode(uint32_t ino) {
    if (ino == 0) return;
    uint32_t group = (ino - 1) / g.inodes_per_group;
    uint32_t index = (ino - 1) % g.inodes_per_group;
    if (group >= g.num_groups) return;

    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    uint32_t imap = g.bgdt[group].bg_inode_bitmap;
    if (!e2_read_block(imap, buf)) return;
    buf[index / 8] &= ~(1u << (index % 8));
    if (!e2_write_block(imap, buf)) return;
    g.bgdt[group].bg_free_inodes_count++;
    (void)e2_flush_bgd(group);
}

/*
 * Remove a directory entry from dir_ino by zeroing its inode field.
 * The rec_len chain is preserved so the space is reclaimed as slack
 * by the next e2_add_dirent scan.
 */
static bool e2_remove_dirent(uint32_t dir_ino, const char *name) {
    ext2_inode_t dir_inode;
    if (!e2_read_inode(dir_ino, &dir_inode)) return false;

    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);

    for (int b = 0; b < 12; b++) {
        uint32_t dblk = dir_inode.i_block[b];
        if (dblk == 0) break;
        if (!e2_read_block(dblk, buf)) continue;

        uint8_t *p   = buf;
        uint8_t *end = buf + g.block_size;
        while (p < end) {
            ext2_dirent_t *de = (ext2_dirent_t*)p;
            if (de->rec_len == 0) break;
            if (de->inode != 0 && e2_name_eq(de, name)) {
                de->inode = 0;
                return e2_write_block(dblk, buf);
            }
            p += de->rec_len;
        }
    }
    return false;
}

/* ── mkdir support ────────────────────────────────────────────────────────── */

/*
 * ext2_mkdir — create a directory on the ext2 disk.
 * path may contain subdirectory components; the parent must already exist.
 * Returns 0 on success, -1 on error.
 */
int ext2_mkdir(const char *path) {
    if (!g.ready || !path || path[0] != '/') return -1;

    char parent[256];
    const char *name;
    uint32_t name_len = e2_split_path(path, parent, sizeof(parent), &name);
    if (name_len == 0) return -1;

    uint32_t parent_ino = e2_resolve(parent);
    if (parent_ino == 0) return -1;

    /* Already exists? */
    if (e2_resolve(path) != 0) return -1;

    /* Allocate inode */
    uint32_t ino = e2_alloc_inode();
    if (ino == 0) return -1;

    /* Allocate one data block */
    uint32_t blk = e2_alloc_block();
    if (!blk) { e2_free_inode(ino); return -1; }

    /* Write '.' and '..' entries into the block */
    uint8_t *buf = (uint8_t*)pmm_phys_to_virt(g.io_phys);
    e2_memset(buf, 0, g.block_size);

    /* '.' entry: rec_len = 12 (smallest aligned entry for 1-char name) */
    ext2_dirent_t *dot = (ext2_dirent_t*)buf;
    dot->inode     = ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = 2; /* EXT2_FT_DIR */
    dot->name[0]   = '.';

    /* '..' entry: rec_len = rest of block */
    ext2_dirent_t *dotdot = (ext2_dirent_t*)(buf + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(g.block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = 2;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    if (!e2_write_block(blk, buf)) { e2_free_inode(ino); e2_free_block(blk); return -1; }

    /* Build and write the directory inode */
    ext2_inode_t inode;
    e2_memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_IMODE_DIR | 0755u;
    inode.i_links_count = 2; /* '.' + parent entry */
    inode.i_size        = g.block_size;
    inode.i_blocks      = g.block_size / 512;
    inode.i_block[0]    = blk;
    {
        uint32_t now = (uint32_t)(pit_get_ticks() / 100);
        inode.i_ctime = now;
        inode.i_mtime = now;
        inode.i_atime = now;
    }

    if (!e2_write_inode(ino, &inode)) { e2_free_inode(ino); e2_free_block(blk); return -1; }

    /* Add directory entry to parent */
    if (!e2_add_dirent(parent_ino, name, name_len, ino, 2 /*EXT2_FT_DIR*/)) {
        e2_free_inode(ino); e2_free_block(blk); return -1;
    }

    return 0;
}

/* ── delete support ───────────────────────────────────────────────────────── */

/*
 * ext2_delete_file — remove a file from the ext2 disk.
 * path may contain subdirectory components.  Frees data blocks and inode.
 * Returns 0 on success, -1 if not found or not a regular file.
 */
int ext2_delete_file(const char *path) {
    if (!g.ready || !path || path[0] != '/') return -1;

    char parent[256];
    const char *name;
    uint32_t name_len = e2_split_path(path, parent, sizeof(parent), &name);
    if (name_len == 0) return -1;
    (void)name_len;

    uint32_t parent_ino = e2_resolve(parent);
    if (parent_ino == 0) return -1;

    uint32_t ino = e2_resolve(path);
    if (ino == 0) return -1;

    ext2_inode_t inode;
    if (!e2_read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & 0xF000u) != EXT2_IMODE_FILE) return -1;

    /* Free direct data blocks */
    for (int b = 0; b < 12; b++) {
        if (inode.i_block[b] == 0) break;
        e2_free_block(inode.i_block[b]);
    }

    /* Free indirect block and its children */
    if (inode.i_block[12] != 0) {
        uint8_t *ibuf = (uint8_t*)pmm_phys_to_virt(g.ind_phys);
        if (e2_read_block(inode.i_block[12], ibuf)) {
            uint32_t *ptrs = (uint32_t*)ibuf;
            uint32_t nptrs = g.block_size / 4;
            for (uint32_t i = 0; i < nptrs; i++) {
                if (ptrs[i] == 0) break;
                e2_free_block(ptrs[i]);
            }
        }
        e2_free_block(inode.i_block[12]);
    }

    /* Remove directory entry first (so partial failures leave no dangling ref) */
    if (!e2_remove_dirent(parent_ino, name)) return -1;

    /* Free the inode */
    e2_free_inode(ino);

    return 0;
}

/* Returns 1 if path names a directory, 0 otherwise. */
int ext2_isdir(const char *path) {
    if (!path) return 0;
    /* Root is always a directory */
    if (path[0] == '/' && path[1] == '\0') return 1;
    uint32_t ino = e2_resolve(path);
    if (!ino) return 0;
    ext2_inode_t inode;
    if (!e2_read_inode(ino, &inode)) return 0;
    return ((inode.i_mode & 0xF000u) == EXT2_IMODE_DIR) ? 1 : 0;
}
