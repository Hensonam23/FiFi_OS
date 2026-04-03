#pragma once
#include <stdint.h>
#include <stddef.h>

/* VFS stage 1: initrd-backed (read-only) */

void vfs_init(void);

/* basic commands */
void vfs_ls(void);
void vfs_cat(const char *path);

/* raw read for later (apps, config, AI models, etc) */
int vfs_read(const char *path, const void **data, uint64_t *size);

/* Write "name\n" lines for every file visible in the VFS into buf.
 * Returns total bytes written (not counting NUL). */
size_t vfs_list(char *buf, size_t cap);

/* Persist a file to ext2 (if disk present).  path must be "/name" or "name".
 * Returns 0 on success, -1 on error or if no ext2 disk. */
int vfs_write(const char *path, const void *data, uint64_t size);

/* Delete a file from ramfs and ext2.  Returns 0 on success, -1 if not found. */
int vfs_delete(const char *path);

/* Create a directory on ext2.  Returns 0 on success, -1 on error. */
int vfs_mkdir(const char *path);

/* Return file size in bytes, or -1 if not found. */
int vfs_filesize(const char *path);
