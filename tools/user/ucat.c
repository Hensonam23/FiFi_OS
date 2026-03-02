#include "usys.h"

static char buf[4096];

int main(void) {
    const char *path = "hello.txt";

    long n = sys_readfile(path, buf, (uint64_t)(sizeof(buf) - 1));
    if (n < 0) {
        sys_log("ucat: sys_readfile failed");
        return 1;
    }

    // print in chunks (avoid kernel-side SYS_WRITE clamp issues)
    uint64_t left = (uint64_t)n;
    uint64_t off = 0;

    while (left) {
        uint64_t chunk = left;
        if (chunk > 512) chunk = 512;
        sys_write(buf + off, chunk);
        off += chunk;
        left -= chunk;
    }

    sys_write("\n", 1);
    return 0;
}
