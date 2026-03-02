#include "usys.h"

int main(void) {
    long fd = sys_open("hello.txt");
    if (fd < 0) {
        sys_log("ucat: open failed");
        return 1;
    }

    char buf[128];

    for (;;) {
        long n = sys_read(fd, buf, (uint64_t)(sizeof(buf) - 1));
        if (n < 0) {
            sys_log("ucat: read failed");
            break;
        }
        if (n == 0) break; // EOF

        buf[n] = 0;
        sys_write(buf, (uint64_t)n);
    }

    sys_close(fd);
    sys_write("\n", 1);
    return 0;
}
