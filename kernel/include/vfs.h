#pragma once
#include <stdint.h>

/* VFS stage 1: initrd-backed (read-only) */

void vfs_init(void);

/* basic commands */
void vfs_ls(void);
void vfs_cat(const char *path);

/* raw read for later (apps, config, AI models, etc) */
int vfs_read(const char *path, const void **data, uint64_t *size);
