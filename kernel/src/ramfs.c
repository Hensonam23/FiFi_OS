#include <stdint.h>
#include <stddef.h>
#include "ramfs.h"
#include "heap.h"

static ramfs_entry_t g_ramfs[RAMFS_MAX_FILES];

static int rf_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return (*a == 0 && *b == 0);
}

static void rf_strcpy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

ramfs_entry_t *ramfs_creat(const char *name) {
    if (!name) return (ramfs_entry_t*)0;
    /* Re-use existing entry with same name (truncate) */
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (g_ramfs[i].used && rf_streq(g_ramfs[i].name, name)) {
            if (g_ramfs[i].data) { kfree(g_ramfs[i].data); g_ramfs[i].data = (uint8_t*)0; }
            g_ramfs[i].size = 0;
            return &g_ramfs[i];
        }
    }
    /* Allocate a fresh slot */
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!g_ramfs[i].used) {
            g_ramfs[i].used = 1;
            g_ramfs[i].size = 0;
            g_ramfs[i].data = (uint8_t*)0;
            rf_strcpy(g_ramfs[i].name, name, RAMFS_NAME_MAX);
            return &g_ramfs[i];
        }
    }
    return (ramfs_entry_t*)0;
}

int ramfs_preallocate(ramfs_entry_t *e, uint32_t cap) {
    if (!e || cap == 0) return -1;
    if (e->data) return 0;   /* already has a buffer */
    e->data = (uint8_t *)kmalloc(cap);
    if (!e->data) return -1;
    e->size = 0;
    return 0;
}

int ramfs_write(const char *name, const void *data, uint32_t len) {
    if (!name || (!data && len > 0)) return -1;

    /* Find or create the slot */
    ramfs_entry_t *e = (ramfs_entry_t*)0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (g_ramfs[i].used && rf_streq(g_ramfs[i].name, name)) {
            e = &g_ramfs[i];
            break;
        }
    }
    if (!e) {
        for (int i = 0; i < RAMFS_MAX_FILES; i++) {
            if (!g_ramfs[i].used) {
                g_ramfs[i].used = 1;
                g_ramfs[i].data = (uint8_t*)0;
                g_ramfs[i].size = 0;
                rf_strcpy(g_ramfs[i].name, name, RAMFS_NAME_MAX);
                e = &g_ramfs[i];
                break;
            }
        }
    }
    if (!e) return -1;   /* ramfs full */

    /* Free previous buffer if any */
    if (e->data) { kfree(e->data); e->data = (uint8_t*)0; }

    if (len == 0) { e->size = 0; return 0; }

    /* Heap-allocate new buffer */
    uint8_t *buf = (uint8_t *)kmalloc(len);
    if (!buf) return -1;

    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) buf[i] = src[i];

    e->data = buf;
    e->size = len;
    return 0;
}

int ramfs_get(const char *name, const void **data, uint64_t *size) {
    if (!name || !data || !size) return -1;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (g_ramfs[i].used && rf_streq(g_ramfs[i].name, name)) {
            *data = (const void*)g_ramfs[i].data;
            *size = (uint64_t)g_ramfs[i].size;
            return 0;
        }
    }
    return -1;
}

int ramfs_delete(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (g_ramfs[i].used && rf_streq(g_ramfs[i].name, name)) {
            if (g_ramfs[i].data) { kfree(g_ramfs[i].data); g_ramfs[i].data = (uint8_t*)0; }
            g_ramfs[i].used = 0;
            g_ramfs[i].size = 0;
            return 0;
        }
    }
    return -1;
}

size_t ramfs_ls_buf(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    size_t pos = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!g_ramfs[i].used) continue;
        const char *n = g_ramfs[i].name;
        for (size_t j = 0; n[j] && pos + 1 < cap; j++) buf[pos++] = n[j];
        if (pos + 1 < cap) buf[pos++] = '\n';
    }
    if (pos < cap) buf[pos] = '\0';
    return pos;
}
