#include "usys.h"

static void segv_handler(int sig) {
    (void)sig;
    sys_log("usegv: SIGSEGV handler called - PASS");
    sys_exit(0);
}

int main(void) {
    sys_log("usegv: registering SIGSEGV handler");
    sys_signal(11, segv_handler);

    sys_log("usegv: triggering NULL deref");
    volatile int *p = (volatile int *)(uintptr_t)0;
    int x = *p;
    (void)x;

    sys_log("usegv: FAIL should not reach here");
    sys_exit(1);
}
