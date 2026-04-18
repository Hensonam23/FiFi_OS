#pragma once
#include <stdint.h>
#include <stddef.h>

#define RAMFS_MAX_FILES  8
#define RAMFS_NAME_MAX   256
#define RAMFS_WR_MAX     (256u * 1024u)  /* max write-redirect buffer (256 KB) */

/*
 * data is heap-allocated on write so the BSS stays small regardless of
 * how large a file is stored.  NULL while the slot is empty.
 */
typedef struct {
    char     name[RAMFS_NAME_MAX];
    uint8_t *data;     /* kmalloc'd; NULL if no data written yet */
    uint32_t size;
    uint8_t  used;
} ramfs_entry_t;

/* Create or truncate a named slot (no data allocated yet). */
ramfs_entry_t *ramfs_creat(const char *name);

/* Pre-allocate a write buffer of cap bytes so incremental writes work. */
int ramfs_preallocate(ramfs_entry_t *e, uint32_t cap);

/* Write data to a named file (heap-allocates the buffer). */
int  ramfs_write(const char *name, const void *data, uint32_t len);

/* Look up a file.  Returns 0 on success and fills *data / *size. */
int  ramfs_get(const char *name, const void **data, uint64_t *size);

/* Delete a named file (frees heap buffer).  Returns 0 on success. */
int  ramfs_delete(const char *name);

/* Fill buf with "name\n" lines for each live entry.  Returns bytes written. */
size_t ramfs_ls_buf(char *buf, size_t cap);
