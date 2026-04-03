#include "usys.h"
#include "ulibc.h"

int main(void) {
    printf("uwait: parent tid=%llu, forking...\n",
           (unsigned long long)sys_gettid());

    long child = sys_fork();

    if (child == 0) {
        /* child */
        printf("[child] tid=%llu sleeping 200ms\n",
               (unsigned long long)sys_gettid());
        sys_sleep_ms(200);
        printf("[child] exiting with code 42\n");
        sys_exit(42);
    }

    /* parent */
    printf("[parent] child tid=%ld, waiting...\n", child);

    int code = 0;
    long reaped = sys_waitpid((unsigned long)child, &code);
    if (reaped > 0) {
        printf("[parent] child %ld exited, code=%d\n", reaped, code);
    } else {
        printf("[parent] waitpid failed (ret=%ld)\n", reaped);
    }

    return 0;
}
