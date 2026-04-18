#include "usys.h"

static void print_hex(unsigned long v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int n = (int)((v >> (i * 4)) & 0xF);
        buf[17 - i] = (char)(n < 10 ? '0' + n : 'a' + n - 10);
    }
    buf[18] = '\0';
    sys_log(buf);
}

int main(void) {
    unsigned long brk0 = sys_brk(0);
    sys_log("ufork2: pre-fork brk=");
    print_hex(brk0);

    /* Advance heap by 2 pages before forking */
    volatile unsigned long *sentinel = (volatile unsigned long *)sys_sbrk(8192);
    if ((long)(uintptr_t)sentinel == -1L) {
        sys_log("ufork2: FAIL sbrk");
        sys_exit(1);
    }
    *sentinel = 0xDEADBEEFCAFEBABEUL;

    unsigned long brk1 = sys_brk(0);
    sys_log("ufork2: post-sbrk brk=");
    print_hex(brk1);

    long r = sys_fork();
    if (r < 0) { sys_log("ufork2: FAIL fork"); sys_exit(1); }

    if (r == 0) {
        /* Child: brk must match parent's post-sbrk brk */
        unsigned long child_brk = sys_brk(0);
        sys_log("ufork2: child brk=");
        print_hex(child_brk);
        if (child_brk != brk1) {
            sys_log("ufork2: FAIL child brk mismatch");
            sys_exit(1);
        }
        /* Inherited heap page must be readable with correct value */
        if (*sentinel != 0xDEADBEEFCAFEBABEUL) {
            sys_log("ufork2: FAIL sentinel corrupted in child");
            sys_exit(1);
        }
        /* Write different value — must not affect parent (isolation check) */
        *sentinel = 0x1234567890ABCDEFUL;
        sys_log("ufork2: child PASS");
        sys_exit(0);
    }

    /* Parent: wait for child */
    int code = -1;
    sys_waitpid((unsigned long)r, &code);

    /* Parent's copy must be unchanged */
    if (*sentinel != 0xDEADBEEFCAFEBABEUL) {
        sys_log("ufork2: FAIL parent sentinel modified by child");
        sys_exit(1);
    }
    sys_log("ufork2: PASS");
    sys_exit(0);
}
