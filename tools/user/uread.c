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
        putstr("usage: uread <file>\n");
        return 1;
    }

    const char *path = argv[1];

    long fd = sys_open(path);
    if (fd < 0) {
        putstr("uread: open failed: ");
        sys_write(path, (uint64_t)c_strlen(path));
        putstr("\n");
        return 2;
    }

    static uint8_t buf[512];

    for (;;) {
        long n = sys_read(fd, buf, (uint64_t)sizeof(buf));
        if (n < 0) {
            putstr("uread: read failed\n");
            sys_close(fd);
            return 3;
        }
        if (n == 0) break;

        sys_write(buf, (uint64_t)n);
    }

    sys_close(fd);
    putstr("\n");
    return 0;
}
