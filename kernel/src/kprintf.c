#include "kprintf.h"
#include "console.h"
#include <stdint.h>

static void putc_(char c) {
    console_putc(c);
}

static void puts_(const char *s) {
    if (!s) s = "(null)";
    while (*s) putc_(*s++);
}

static void utoa_dec(uint64_t v, char *buf) {
    char tmp[32];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void utoa_hex(uint64_t v, char *buf) {
    static const char *hex = "0123456789abcdef";
    char tmp[32];
    int i = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0) {
        tmp[i++] = hex[v & 0xF];
        v >>= 4;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void kvprintf(const char *fmt, va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            putc_(*p);
            continue;
        }

        p++;
        if (*p == 0) break;

        if (*p == '%') { putc_('%'); continue; }

        if (*p == 'c') {
            char c = (char)va_arg(args, int);
            putc_(c);
            continue;
        }

        if (*p == 's') {
            const char *s = va_arg(args, const char *);
            puts_(s);
            continue;
        }

        if (*p == 'd') {
            int64_t v = (int64_t)va_arg(args, int);
            if (v < 0) { putc_('-'); v = -v; }
            char buf[32];
            utoa_dec((uint64_t)v, buf);
            puts_(buf);
            continue;
        }

        if (*p == 'u') {
            uint64_t v = (uint64_t)va_arg(args, unsigned int);
            char buf[32];
            utoa_dec(v, buf);
            puts_(buf);
            continue;
        }

        if (*p == 'x') {
            uint64_t v = (uint64_t)va_arg(args, unsigned int);
            char buf[32];
            utoa_hex(v, buf);
            puts_(buf);
            continue;
        }

        if (*p == 'p') {
            uintptr_t v = (uintptr_t)va_arg(args, void *);
            puts_("0x");
            char buf[32];
            utoa_hex((uint64_t)v, buf);
            puts_(buf);
            continue;
        }

        // Unknown format: print it literally
        putc_('%');
        putc_(*p);
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
