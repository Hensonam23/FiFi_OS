#include "usys.h"
#include <stdint.h>
#include <stddef.h>

static size_t c_strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void putstr(const char *s) {
    sys_write(s, (uint64_t)c_strlen(s));
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv || !argv[1]) {
        putstr("usage: ucat <file>\n");
        return 1;
    }

    const char *path = argv[1];

    // Big static buffer so we don't blow the user stack.
    static uint8_t buf[64 * 1024];

    long n = sys_readfile(path, buf, (uint64_t)sizeof(buf));
    if (n < 0) {
        putstr("ucat: not found: ");
        sys_write(path, (uint64_t)c_strlen(path));
        putstr("\n");
        return 2;
    }

    if (n > 0) {
        sys_write(buf, (uint64_t)n);
        if (buf[(size_t)n - 1] != '\n') {
            putstr("\n");
        }
    }

    return 0;
}
