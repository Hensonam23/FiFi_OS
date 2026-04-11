#include <stdint.h>
#include <stddef.h>

#include "vfs.h"
#include "initrd.h"
#include "ext2.h"
#include "ramfs.h"
#include "heap.h"
#include "kprintf.h"

static const char *vfs_norm_path(const char *p) {
    if (!p) return 0;
    while (*p == '/') p++;
    return p;
}

/*
 * Static read buffer for ext2-sourced files.  Safe to reuse between calls
 * because the kernel is cooperative: callers (exec_load, run_thread_fn) fully
 * consume the data before yielding.  64 KiB covers any ELF we'd put on disk.
 */
#define EXT2_BUF_CAP (64u * 1024u)
static void    *g_ext2_buf = (void*)0;

void vfs_init(void) {
    initrd_init();
}

void vfs_ls(void) {
    initrd_ls();
}

int vfs_read(const char *path, const void **data, uint64_t *size) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;

    /* ramfs first (written files shadow initrd/ext2) */
    if (ramfs_get(n, data, size) == 0) return 0;

    /* Try initrd */
    if (initrd_get(n, data, size) == 0) return 0;

    /* Fall back to ext2 */
    if (!ext2_present()) return -1;

    /* Lazy-allocate the static ext2 read buffer */
    if (!g_ext2_buf) {
        g_ext2_buf = kmalloc(EXT2_BUF_CAP);
        if (!g_ext2_buf) return -1;
    }

    /* Reconstruct absolute path: prepend '/' that vfs_norm_path stripped */
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
    full[i + 1] = '\0';

    int got = ext2_read_file(full, g_ext2_buf, EXT2_BUF_CAP);
    if (got < 0) return -1;

    *data = g_ext2_buf;
    *size = (uint64_t)got;
    return 0;
}

/* Return 1 if name (nlen chars) already appears as a "name\n" entry in buf. */
static int vfs_name_seen(const char *buf, size_t pos, const char *name, size_t nlen) {
    for (size_t i = 0; i + nlen < pos; i++) {
        if ((i == 0 || buf[i - 1] == '\n') && buf[i + nlen] == '\n') {
            size_t j = 0;
            while (j < nlen && buf[i + j] == name[j]) j++;
            if (j == nlen) return 1;
        }
    }
    return 0;
}

/*
 * Collect all three layers into a temporary buffer, then copy entries into
 * buf while skipping duplicates (ramfs shadows initrd shadows ext2).
 */
static char g_list_tmp[8192];

size_t vfs_list(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;

    size_t raw = 0;
    raw += ramfs_ls_buf(g_list_tmp + raw, sizeof(g_list_tmp) - raw);
    raw += initrd_ls_buf(g_list_tmp + raw, sizeof(g_list_tmp) - raw);
    if (ext2_present())
        raw += ext2_ls_buf(g_list_tmp + raw, sizeof(g_list_tmp) - raw);

    /* Copy entries, skipping duplicates */
    size_t pos = 0;
    const char *p = g_list_tmp;
    const char *end = g_list_tmp + raw;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        if (nl >= end) break;
        size_t nlen = (size_t)(nl - p);
        if (!vfs_name_seen(buf, pos, p, nlen) && pos + nlen + 1 < cap) {
            for (size_t j = 0; j < nlen; j++) buf[pos++] = p[j];
            buf[pos++] = '\n';
        }
        p = nl + 1;
    }
    if (pos < cap) buf[pos] = '\0';
    return pos;
}

int vfs_mkdir(const char *path) {
    if (!ext2_present()) return -1;
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
    full[i + 1] = '\0';
    return ext2_mkdir(full);
}

/* Returns file size in bytes, or -1 if not found. */
int vfs_filesize(const char *path) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;
    const void *data; uint64_t size;
    if (ramfs_get(n, &data, &size) == 0) return (int)size;
    if (ext2_present()) {
        char full[258];
        full[0] = '/';
        size_t i = 0;
        while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
        full[i + 1] = '\0';
        return ext2_file_size(full);
    }
    return -1;
}

int vfs_delete(const char *path) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;
    /* Remove from ramfs (session layer) */
    (void)ramfs_delete(n);
    /* Remove from ext2 (persistent layer) */
    if (!ext2_present()) return 0;
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
    full[i + 1] = '\0';
    return ext2_delete_file(full);
}

int vfs_write(const char *path, const void *data, uint64_t size) {
    if (!ext2_present()) return -1;
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
    full[i + 1] = '\0';
    return ext2_write_file(full, data, (uint32_t)size);
}

/* Returns 1 if path is a directory (initrd and ramfs are flat, so only ext2 +
 * the virtual root "/" are checked). */
int vfs_isdir(const char *path) {
    const char *n = vfs_norm_path(path);
    /* Normalised empty string means the caller passed "/" — that's the root */
    if (!n || !*n) return 1;
    if (!ext2_present()) return 0;
    char full[258];
    full[0] = '/';
    size_t i = 0;
    while (n[i] && i < 256) { full[i + 1] = n[i]; i++; }
    full[i + 1] = '\0';
    return ext2_isdir(full);
}

void vfs_cat(const char *path) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) {
        kprintf("usage: cat <file>\n");
        return;
    }
    initrd_cat(n);
}
