#include <stdint.h>
#include <stddef.h>

#include "vfs.h"
#include "initrd.h"
#include "ext2.h"
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

    /* Try initrd first */
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

size_t vfs_list(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    size_t pos = initrd_ls_buf(buf, cap);
    if (ext2_present() && pos < cap)
        pos += ext2_ls_buf(buf + pos, cap - pos);
    if (pos < cap) buf[pos] = '\0';
    return pos;
}

void vfs_cat(const char *path) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) {
        kprintf("usage: cat <file>\n");
        return;
    }
    initrd_cat(n);
}
