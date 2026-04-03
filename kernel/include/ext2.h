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
