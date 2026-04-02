#include "usys.h"

int main(void) {
    sys_log("ufork: calling fork");
    long r = sys_fork();
    if (r < 0) {
        sys_log("ufork: fork failed");
        sys_exit(1);
    }
    if (r == 0) {
        sys_log("ufork: I am the child (rax=0)");
        sys_exit(0);
    }
    sys_log("ufork: I am the parent, child spawned");
    sys_exit(0);
}
