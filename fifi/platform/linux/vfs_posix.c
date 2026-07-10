#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "vfs.h"

/* VFS root — override with -DVFS_ROOT='"path"' at compile time */
#ifndef VFS_ROOT
#define VFS_ROOT "/fifi-data"
#endif

/* Build a full path under VFS_ROOT, normalizing . and .. so callers cannot
 * escape the data root with paths like /../../etc/passwd. Returns false and
 * sets out to VFS_ROOT if the normalized path would escape the root. */
static bool make_full(char *out, size_t cap, const char *path) {
    char raw[512];
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        snprintf(raw, sizeof(raw), "%s", VFS_ROOT);
    } else if (path[0] == '/') {
        snprintf(raw, sizeof(raw), "%s%s", VFS_ROOT, path);
    } else {
        snprintf(raw, sizeof(raw), "%s/%s", VFS_ROOT, path);
    }

    /* Normalize: collapse //, /./, and /.. in place */
    char norm[512];
    int ni = 0;
    const char *p = raw;
    while (*p) {
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == '\0')) {
            /* /.. — go up one level */
            if (ni > 0 && norm[ni-1] == '/') ni--;
            while (ni > 0 && norm[ni-1] != '/') ni--;
            p += 3;
        } else if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            /* /.  — skip */
            p += 2;
        } else if (p[0] == '/' && p[1] == '/') {
            /* // — collapse */
            p++;
        } else {
            if (ni < (int)sizeof(norm) - 1) norm[ni++] = *p;
            p++;
        }
    }
    norm[ni] = '\0';
    if (ni == 0) { norm[0] = '/'; norm[1] = '\0'; }

    /* Verify the normalized path is still under VFS_ROOT */
    size_t root_len = strlen(VFS_ROOT);
    if (strncmp(norm, VFS_ROOT, root_len) != 0 ||
        (norm[root_len] != '\0' && norm[root_len] != '/')) {
        snprintf(out, cap, "%s", VFS_ROOT);
        return false;
    }

    snprintf(out, cap, "%s", norm);
    return true;
}

/* Resolve a VFS path ("/fonts/x.ttf") to a real host path under VFS_ROOT so
 * callers that use raw fopen (e.g. the TTF rasterizer) hit the same files as
 * vfs_read. Returns the length written. */
size_t vfs_real_path(const char *path, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    make_full(out, cap, path);
    size_t n = 0; while (out[n] && n < cap) n++;
    return n;
}

/* Read-buffer cache — small pool so vfs_read pointers stay valid */
#define RBUF_SLOTS 16
#define RBUF_MAX   (512 * 1024)  /* 512 KB per file */
static struct {
    uint8_t *data;
    uint64_t size;
} g_rbufs[RBUF_SLOTS];
static int g_rbuf_next = 0;

void vfs_init(void) {
    /* Ensure VFS root exists */
    mkdir(VFS_ROOT, 0755);
}

int vfs_read(const char *path, const void **data, uint64_t *size) {
    char full[512];
    make_full(full, sizeof(full), path);

    FILE *f = fopen(full, "rb");
    if (!f) { if (data) *data = NULL; if (size) *size = 0; return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz < 0 || sz > RBUF_MAX) { fclose(f); return -1; }

    int slot = g_rbuf_next % RBUF_SLOTS;
    free(g_rbufs[slot].data);
    g_rbufs[slot].data = (uint8_t *)malloc((size_t)sz + 1);
    if (!g_rbufs[slot].data) { fclose(f); return -1; }
    g_rbufs[slot].size = (uint64_t)sz;
    g_rbufs[slot].data[sz] = 0;
    fread(g_rbufs[slot].data, 1, (size_t)sz, f);
    fclose(f);
    g_rbuf_next++;

    if (data) *data = g_rbufs[slot].data;
    if (size) *size = (uint64_t)sz;
    return 0;
}

int vfs_write(const char *path, const void *data, uint64_t size) {
    char full[512];
    make_full(full, sizeof(full), path);

    FILE *f = fopen(full, "wb");
    if (!f) return -1;
    if (size > 0 && data) fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return 0;
}

int vfs_delete(const char *path) {
    char full[512];
    make_full(full, sizeof(full), path);
    return remove(full) == 0 ? 0 : -1;
}

int vfs_rename(const char *old_path, const char *new_path) {
    char old_full[512], new_full[512];
    make_full(old_full, sizeof(old_full), old_path);
    make_full(new_full, sizeof(new_full), new_path);
    return rename(old_full, new_full) == 0 ? 0 : -1;
}

int vfs_mkdir(const char *path) {
    char full[512];
    make_full(full, sizeof(full), path);
    return mkdir(full, 0755) == 0 ? 0 : -1;
}

int vfs_filesize(const char *path) {
    char full[512];
    make_full(full, sizeof(full), path);
    struct stat st;
    if (stat(full, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) return -1;
    return (int)st.st_size;
}

int vfs_isdir(const char *path) {
    char full[512];
    make_full(full, sizeof(full), path);
    struct stat st;
    if (stat(full, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

size_t vfs_list(char *buf, size_t cap) {
    return vfs_listdir("/", buf, cap);
}

size_t vfs_listdir(const char *path, char *buf, size_t cap) {
    char full[512];
    make_full(full, sizeof(full), path);

    DIR *d = opendir(full);
    if (!d) return 0;

    size_t written = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;  /* skip hidden / . / .. */
        size_t nlen = strlen(name);
        if (written + nlen + 2 >= cap) break;
        memcpy(buf + written, name, nlen);
        written += nlen;
        buf[written++] = '\n';
    }
    closedir(d);
    if (written < cap) buf[written] = '\0';
    return written;
}

/* Stubs for bare-metal-only diagnostics */
void vfs_ls(void)             { }
void vfs_cat(const char *p)   { (void)p; }
