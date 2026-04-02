#include "usys.h"

/* Minimal hex printer (no libc) */
static void print_hex(unsigned long v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; i--) {
        int nibble = (int)((v >> (i * 4)) & 0xF);
        buf[17 - i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buf[18] = '\0';
    sys_log(buf);
}

int main(void) {
    /* Query initial brk */
    unsigned long brk0 = sys_brk(0);
    sys_log("uheap: initial brk =");
    print_hex(brk0);

    /* Allocate 3 pages */
    void *p1 = sys_sbrk(4096);
    void *p2 = sys_sbrk(4096);
    void *p3 = sys_sbrk(4096);

    if (p1 == (void*)-1UL || p2 == (void*)-1UL || p3 == (void*)-1UL) {
        sys_log("uheap: sbrk FAILED");
        sys_exit(1);
    }

    sys_log("uheap: allocated 3 pages OK");

    /* Write distinct sentinel values to each page */
    volatile unsigned long *a = (volatile unsigned long*)p1;
    volatile unsigned long *b = (volatile unsigned long*)p2;
    volatile unsigned long *c = (volatile unsigned long*)p3;
    *a = 0xAAAAAAAAAAAAAAAAUL;
    *b = 0xBBBBBBBBBBBBBBBBUL;
    *c = 0xCCCCCCCCCCCCCCCCUL;

    /* Read back and verify */
    if (*a != 0xAAAAAAAAAAAAAAAAUL) { sys_log("uheap: page1 CORRUPT"); sys_exit(1); }
    if (*b != 0xBBBBBBBBBBBBBBBBUL) { sys_log("uheap: page2 CORRUPT"); sys_exit(1); }
    if (*c != 0xCCCCCCCCCCCCCCCCUL) { sys_log("uheap: page3 CORRUPT"); sys_exit(1); }

    sys_log("uheap: all pages verified OK");

    unsigned long brk1 = sys_brk(0);
    sys_log("uheap: brk after alloc =");
    print_hex(brk1);

    /* Shrink back to original brk */
    unsigned long brk2 = sys_brk(brk0);
    sys_log("uheap: brk after shrink =");
    print_hex(brk2);

    sys_log("uheap: done");
    sys_exit(0);
}
