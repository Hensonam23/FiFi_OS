#pragma once
#include <stdint.h>
#include <stddef.h>

#define RAMFS_MAX_FILES  8
#define RAMFS_FILE_SIZE  4096
#define RAMFS_NAME_MAX   64

typedef struct {
    char     name[RAMFS_NAME_MAX];
    uint8_t  data[RAMFS_FILE_SIZE];
    uint32_t size;
    uint8_t  used;
} ramfs_entry_t;

/* Create or truncate a named file.  Returns entry pointer, or NULL on error. */
ramfs_entry_t *ramfs_creat(const char *name);

/* Look up a file.  Returns 0 on success and fills *data / *size. */
int  ramfs_get(const char *name, const void **data, uint64_t *size);

/* Fill buf with "name\n" lines for each live entry.  Returns bytes written. */
size_t ramfs_ls_buf(char *buf, size_t cap);
