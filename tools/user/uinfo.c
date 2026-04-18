#include "usys.h"
#include "ulibc.h"

static uint8_t g_buf[512];

int main(void) {
    printf("=== uinfo ===\n");
    printf("tid    : %llu\n", (unsigned long long)sys_gettid());
    printf("pid    : %lu\n",  (unsigned long)sys_getpid());
    printf("ppid   : %lu\n",  (unsigned long)sys_getppid());
    printf("uptime : %llu ms\n", (unsigned long long)sys_uptime());
    printf("time   : %lu s\n",   (unsigned long)sys_time());

    struct fifi_utsname u;
    if (sys_uname(&u) == 0) {
        printf("sysname: %s\n", u.sysname);
        printf("release: %s\n", u.release);
        printf("machine: %s\n", u.machine);
        printf("version: %s\n", u.version);
    }

    /* stat a known file */
    struct fifi_stat st;
    if (sys_stat("motd.txt", &st) == 0)
        printf("stat   : motd.txt  %llu bytes\n", (unsigned long long)st.size);

    /* read hello.txt from ext2 if present */
    long n = sys_readfile("/hello.txt", g_buf, (uint64_t)(sizeof(g_buf) - 1));
    if (n > 0) {
        g_buf[(size_t)n] = '\0';
        while (n > 0 && (g_buf[n-1] == '\n' || g_buf[n-1] == '\r')) g_buf[--n] = '\0';
        printf("disk   : \"%s\" (%ld bytes)\n", (char*)g_buf, n);
    } else {
        printf("disk   : (no /hello.txt)\n");
    }

    printf("done.\n");
    return 0;
}
