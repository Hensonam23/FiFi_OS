#include "usys.h"
#include "ulibc.h"

static char buf[4096];

int main(void) {
    long n = sys_listfiles(buf, sizeof(buf));
    if (n < 0) {
        printf("uls: listfiles failed\n");
        return 1;
    }
    sys_write(buf, (uint64_t)n);
    return 0;
}
