#include <stdint.h>

#include "vfs.h"
#include "initrd.h"
#include "kprintf.h"

static const char *vfs_norm_path(const char *p) {
    if (!p) return 0;
    while (*p == '/') p++;
    return p;
}

void vfs_init(void) {
    /* Stage 1: mount/init initrd */
    initrd_init();
}

void vfs_ls(void) {
    initrd_ls();
}

int vfs_read(const char *path, const void **data, uint64_t *size) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) return -1;
    return initrd_get(n, data, size);
}

void vfs_cat(const char *path) {
    const char *n = vfs_norm_path(path);
    if (!n || !*n) {
        kprintf("usage: cat <file>\n");
        return;
    }
    initrd_cat(n);
}
