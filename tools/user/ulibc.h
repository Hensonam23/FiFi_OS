#pragma once
/*
 * ulibc.h — single-header userland libc for FiFi OS
 *
 * Provides: printf, sprintf, snprintf, vsnprintf
 *           strlen, strcmp, strncmp, strcpy, strncpy, strcat
 *           memcpy, memset, memcmp
 *           putchar, puts
 *
 * All output goes through sys_write (no buffering beyond the format buffer).
 * Include this header in exactly one .c file with ULIBC_IMPL defined to get
 * the implementations; other files just get the declarations.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "usys.h"

/* ── declarations ─────────────────────────────────────────────────────────── */

static inline size_t ul_strlen(const char *s);
static inline int    ul_strcmp(const char *a, const char *b);
static inline int    ul_strncmp(const char *a, const char *b, size_t n);
static inline char  *ul_strcpy(char *dst, const char *src);
static inline char  *ul_strncpy(char *dst, const char *src, size_t n);
static inline char  *ul_strcat(char *dst, const char *src);
static inline char  *ul_strchr(const char *s, int c);
static inline void  *ul_memcpy(void *dst, const void *src, size_t n);
static inline void  *ul_memset(void *s, int c, size_t n);
static inline int    ul_memcmp(const void *a, const void *b, size_t n);

int  ul_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  ul_snprintf(char *buf, size_t cap, const char *fmt, ...);
int  ul_sprintf(char *buf, const char *fmt, ...);
int  ul_printf(const char *fmt, ...);
void ul_putchar(char c);
void ul_puts(const char *s);

/* Convenience aliases so callers can write printf(...) instead of ul_printf */
#define printf    ul_printf
#define sprintf   ul_sprintf
#define snprintf  ul_snprintf
#define vsnprintf ul_vsnprintf
#define strlen    ul_strlen
#define strcmp    ul_strcmp
#define strncmp   ul_strncmp
#define strcpy    ul_strcpy
#define strncpy   ul_strncpy
#define strcat    ul_strcat
#define strchr    ul_strchr
#define memcpy    ul_memcpy
#define memset    ul_memset
#define memcmp    ul_memcmp
#define putchar   ul_putchar
#define puts      ul_puts

/* ── inline string/memory helpers ────────────────────────────────────────── */

static inline size_t ul_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static inline int ul_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline int ul_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static inline char *ul_strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static inline char *ul_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

static inline char *ul_strcat(char *dst, const char *src) {
    char *d = dst + ul_strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

static inline char *ul_strchr(const char *s, int c) {
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char*)s;
    return (char*)0;
}

static inline void *ul_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static inline void *ul_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t*)s;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
    return s;
}

static inline int ul_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t*)a, *q = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    }
    return 0;
}

/* ── vsnprintf implementation ────────────────────────────────────────────── */

static inline void _ul_emit(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos + 1 < cap) buf[*pos] = c;
    (*pos)++;
}

static inline void _ul_emit_str(char *buf, size_t cap, size_t *pos,
                                const char *s, int width, int left, char pad) {
    if (!s) s = "(null)";
    size_t len = ul_strlen(s);
    int w = (width > 0 && (size_t)width > len) ? width - (int)len : 0;
    if (!left) for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, pad);
    for (size_t i = 0; i < len; i++) _ul_emit(buf, cap, pos, s[i]);
    if (left)  for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, ' ');
}

static inline void _ul_emit_uint(char *buf, size_t cap, size_t *pos,
                                 uint64_t v, int base, int upper,
                                 int width, int left, char pad, int alt) {
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digs = upper ? hi : lo;
    char tmp[66];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = digs[v % (uint64_t)base]; v /= (uint64_t)base; } }
    /* prefix for hex alt-form */
    char pfx[3];
    pfx[0] = pfx[1] = pfx[2] = 0;
    int pfxlen = 0;
    if (alt && base == 16 && !(n == 1 && tmp[0] == '0')) {
        pfx[0] = '0'; pfx[1] = upper ? 'X' : 'x'; pfxlen = 2;
    }
    int total = n + pfxlen;
    int w = (width > total) ? width - total : 0;
    if (!left && pad == ' ') for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, ' ');
    for (int i = 0; i < pfxlen; i++) _ul_emit(buf, cap, pos, pfx[i]);
    if (!left && pad == '0') for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, '0');
    for (int i = n - 1; i >= 0; i--) _ul_emit(buf, cap, pos, tmp[i]);
    if (left) for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, ' ');
}

static inline void _ul_emit_int(char *buf, size_t cap, size_t *pos,
                                int64_t v, int width, int left, char pad) {
    char tmp[22];
    int n = 0;
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    if (u == 0) { tmp[n++] = '0'; }
    else { while (u) { tmp[n++] = '0' + (int)(u % 10); u /= 10; } }
    int total = n + (neg ? 1 : 0);
    int w = (width > total) ? width - total : 0;
    if (!left && pad == ' ') for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, ' ');
    if (neg) _ul_emit(buf, cap, pos, '-');
    if (!left && pad == '0') for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, '0');
    for (int i = n - 1; i >= 0; i--) _ul_emit(buf, cap, pos, tmp[i]);
    if (left) for (int i = 0; i < w; i++) _ul_emit(buf, cap, pos, ' ');
}

int ul_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (!buf || cap == 0) return 0;

    while (*fmt) {
        if (*fmt != '%') { _ul_emit(buf, cap, &pos, *fmt++); continue; }
        fmt++; /* skip '%' */

        /* flags */
        int left = 0, alt = 0;
        char pad = ' ';
        while (*fmt == '-' || *fmt == '0' || *fmt == '#') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') pad = '0';
            if (*fmt == '#') alt = 1;
            fmt++;
        }
        if (left) pad = ' '; /* left-align ignores zero-pad */

        /* width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt++ - '0'); }

        /* length modifier: l, ll, z */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'z') { is_long = 2; fmt++; }

        char spec = *fmt++;
        switch (spec) {
        case 'd': case 'i': {
            int64_t v = (is_long == 2) ? (int64_t)va_arg(ap, long long)
                      : (is_long == 1) ? (int64_t)va_arg(ap, long)
                                       : (int64_t)va_arg(ap, int);
            _ul_emit_int(buf, cap, &pos, v, width, left, pad);
            break;
        }
        case 'u': {
            uint64_t v = (is_long == 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                                        : (uint64_t)va_arg(ap, unsigned int);
            _ul_emit_uint(buf, cap, &pos, v, 10, 0, width, left, pad, 0);
            break;
        }
        case 'x': case 'X': {
            uint64_t v = (is_long == 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                                        : (uint64_t)va_arg(ap, unsigned int);
            _ul_emit_uint(buf, cap, &pos, v, 16, spec == 'X', width, left, pad, alt);
            break;
        }
        case 'o': {
            uint64_t v = (is_long == 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                                        : (uint64_t)va_arg(ap, unsigned int);
            _ul_emit_uint(buf, cap, &pos, v, 8, 0, width, left, pad, 0);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void*);
            _ul_emit(buf, cap, &pos, '0');
            _ul_emit(buf, cap, &pos, 'x');
            _ul_emit_uint(buf, cap, &pos, (uint64_t)v, 16, 0, width, left, '0', 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char*);
            _ul_emit_str(buf, cap, &pos, s, width, left, pad);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            _ul_emit(buf, cap, &pos, c);
            break;
        }
        case '%':
            _ul_emit(buf, cap, &pos, '%');
            break;
        default:
            _ul_emit(buf, cap, &pos, '%');
            _ul_emit(buf, cap, &pos, spec);
            break;
        }
    }

    /* always NUL-terminate */
    buf[pos < cap ? pos : cap - 1] = '\0';
    return (int)pos;
}

int ul_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = ul_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int ul_sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = ul_vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

int ul_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = ul_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r > 0) {
        size_t len = (size_t)r < sizeof(buf) ? (size_t)r : sizeof(buf) - 1;
        sys_write(buf, (uint64_t)len);
    }
    return r;
}

void ul_putchar(char c) {
    sys_write(&c, 1);
}

void ul_puts(const char *s) {
    if (!s) return;
    sys_write(s, (uint64_t)ul_strlen(s));
    sys_write("\n", 1);
}

/* Pull in the heap allocator (malloc/free/calloc/realloc) */
#include "umalloc.h"
