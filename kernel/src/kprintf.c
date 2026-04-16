#include "kprintf.h"
#include "console.h"
#include "serial.h"
#include <stdint.h>
#include "spinlock.h"

static spinlock_t g_kprintf_lock = {0};


/* === print-state tracking (for clean shell prompt redraw) === */
static volatile int g_print_input_active = 0;
static volatile int g_print_dirty = 0;
static volatile int g_print_suppress_dirty = 0;

void print_set_input_active(int on) {
    __atomic_store_n(&g_print_input_active, on ? 1 : 0, __ATOMIC_RELAXED);
}

void print_set_suppress_dirty(int on) {
    __atomic_store_n(&g_print_suppress_dirty, on ? 1 : 0, __ATOMIC_RELAXED);
}

int print_take_dirty(void) {
    int d = __atomic_exchange_n(&g_print_dirty, 0, __ATOMIC_ACQ_REL);
    return d ? 1 : 0;
}
/* === end print-state === */


static void putc_(char c) {
    console_putc(c);
    if (c == '\n') serial_write_char('\r');
    serial_write_char(c);
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

        /* Parse flags and width: supports %0Nd, %0Nx, %Ns, %.Ns */
        int zero_pad = 0;
        int width = 0;
        int precision = -1;

        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }

        if (*p == 0) break;

        if (*p == 'c') {
            char c = (char)va_arg(args, int);
            putc_(c);
            continue;
        }

        if (*p == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            if (precision >= 0) {
                for (int i = 0; i < precision && s[i]; i++)
                    putc_(s[i]);
            } else {
                puts_(s);
            }
            continue;
        }

        if (*p == 'd') {
            int64_t v = (int64_t)va_arg(args, int);
            char buf[32];
            int neg = 0;
            if (v < 0) { neg = 1; v = -v; }
            utoa_dec((uint64_t)v, buf);
            int len = 0;
            while (buf[len]) len++;
            if (neg) putc_('-');
            for (int i = len; i < width; i++)
                putc_(zero_pad ? '0' : ' ');
            puts_(buf);
            continue;
        }

        if (*p == 'u') {
            uint64_t v = (uint64_t)va_arg(args, unsigned int);
            char buf[32];
            utoa_dec(v, buf);
            int len = 0;
            while (buf[len]) len++;
            for (int i = len; i < width; i++)
                putc_(zero_pad ? '0' : ' ');
            puts_(buf);
            continue;
        }

        if (*p == 'x') {
            uint64_t v = (uint64_t)va_arg(args, unsigned int);
            char buf[32];
            utoa_hex(v, buf);
            int len = 0;
            while (buf[len]) len++;
            for (int i = len; i < width; i++)
                putc_(zero_pad ? '0' : ' ');
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
    uint64_t __kpf = spin_lock_irqsave(&g_kprintf_lock);
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
        if (__atomic_load_n(&g_print_input_active, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&g_print_suppress_dirty, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_print_dirty, 1, __ATOMIC_RELAXED);
    }
spin_unlock_irqrestore(&g_kprintf_lock, __kpf);
}
