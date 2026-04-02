#include "usys.h"

/* uexec: exec into /ulog.elf — if exec works, ulog prints "hi from ulog.elf"
 * and exits. If exec fails we see this error message instead. */
int main(void) {
    sys_log("uexec: calling exec /ulog.elf");
    long r = sys_exec("/ulog.elf");
    /* only reaches here on failure */
    sys_log("uexec: exec FAILED");
    (void)r;
    sys_exit(1);
}
