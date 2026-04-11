#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Initialize ext2 from the VirtIO block device.
 * Returns false if no disk, bad magic, or allocation failure. */
bool ext2_init(void);

bool ext2_present(void);

/* Read a file into buf (max bytes).  Returns bytes read, or -1 on error. */
int ext2_read_file(const char *path, void *buf, uint32_t max);

/* List a directory (prints to kprintf). */
void ext2_ls(const char *path);

/* Return file size in bytes, or -1 if not found / not a file. */
int ext2_file_size(const char *path);

/* Write "name\n" entries for root directory into buf. Returns bytes written. */
size_t ext2_ls_buf(char *buf, size_t cap);

/* Create or overwrite a root-level file on disk.
 * path must be "/filename" (no subdirectories).
 * Supports files up to 12 * block_size bytes (direct blocks only).
 * Returns 0 on success, -1 on error. */
int ext2_write_file(const char *path, const void *data, uint32_t size);

/* Create a root-level directory on disk.
 * path must be "/dirname".
 * Returns 0 on success, -1 on error or if already exists. */
int ext2_mkdir(const char *path);

/* Delete a root-level regular file from disk.
 * path must be "/filename".
 * Returns 0 on success, -1 if not found or error. */
int ext2_delete_file(const char *path);

/* Returns 1 if path is a directory, 0 if not found or not a directory. */
int ext2_isdir(const char *path);
