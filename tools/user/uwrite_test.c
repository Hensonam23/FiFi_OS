#include "usys.h"

static void pstr(const char *s) { sys_log(s); }

static void pfile(const char *path) {
    static char buf[512];
    long n = sys_readfile(path, buf, sizeof(buf) - 1);
    if (n < 0) { pstr("(read failed)"); return; }
    buf[n] = '\0';
    pstr(buf);
}

int main(void) {
    /* Test 1: root-level write */
    pstr("uwrite_test: TEST1 root write");
    int fd = sys_creat("/uwrite_out.txt");
    if (fd < 0) { pstr("FAIL creat"); sys_exit(1); }
    const char *msg = "hello from uwrite\n";
    long wlen = 0;
    while (msg[wlen]) wlen++;
    sys_fdwrite(fd, msg, (uint64_t)wlen);
    sys_close(fd);
    pstr("uwrite_test: root write done, reading back:");
    pfile("/uwrite_out.txt");

    /* Test 2: append */
    pstr("uwrite_test: TEST2 append");
    int fd2 = sys_openw("/uwrite_out.txt");
    if (fd2 < 0) { pstr("FAIL openw"); sys_exit(1); }
    const char *app = "appended line\n";
    long alen = 0;
    while (app[alen]) alen++;
    sys_fdwrite(fd2, app, (uint64_t)alen);
    sys_close(fd2);
    pstr("uwrite_test: after append:");
    pfile("/uwrite_out.txt");

    /* Test 3: subdirectory write with auto-mkdir */
    pstr("uwrite_test: TEST3 subdir write (auto-mkdir)");
    int fd3 = sys_creat("/autodir/nested.txt");
    if (fd3 < 0) { pstr("FAIL subdir creat"); sys_exit(1); }
    const char *sub = "subdir content\n";
    long slen = 0;
    while (sub[slen]) slen++;
    sys_fdwrite(fd3, sub, (uint64_t)slen);
    sys_close(fd3);
    pstr("uwrite_test: subdir write done, reading back:");
    pfile("/autodir/nested.txt");

    pstr("uwrite_test: ALL PASS");
    sys_exit(0);
}
