#include "usys.h"
#include "ulibc.h"

static uint8_t g_buf[512];

int main(void) {
    uint64_t tid  = sys_gettid();
    uint64_t upms = sys_uptime();

    printf("=== uinfo ===\n");
    printf("tid    : %llu\n", (unsigned long long)tid);
    printf("uptime : %llu ms\n", (unsigned long long)upms);

    /* Read a small file from the ext2 disk image */
    const char *path = "/hello.txt";
    long n = sys_readfile(path, g_buf, (uint64_t)(sizeof(g_buf) - 1));
    if (n > 0) {
        g_buf[(size_t)n] = '\0';
        /* trim trailing newlines for cleaner output */
        while (n > 0 && (g_buf[n-1] == '\n' || g_buf[n-1] == '\r')) {
            g_buf[--n] = '\0';
        }
        printf("disk   : \"%s\" (%ld bytes)\n", (char*)g_buf, n);
    } else {
        printf("disk   : (no /hello.txt — is disk.img mounted?)\n");
    }

    /* Also try the initrd motd */
    const char *motd = "motd.txt";
    long m = sys_readfile(motd, g_buf, (uint64_t)(sizeof(g_buf) - 1));
    if (m > 0) {
        g_buf[(size_t)m] = '\0';
        printf("motd   : %ld bytes read from initrd\n", m);
    }

    printf("done.\n");
    return 0;
}
