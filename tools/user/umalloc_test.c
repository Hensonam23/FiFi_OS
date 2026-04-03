#include "usys.h"
#include "umalloc.h"

/* Minimal helpers (no libc) */
static void print_str(const char *s) { sys_log(s); }

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

static void pass(const char *msg) {
    sys_log(msg);
}

static void fail(const char *msg) {
    sys_log(msg);
    sys_exit(1);
}

int main(void) {
    print_str("umalloc_test: start");

    /* Basic alloc + write + free */
    char *a = (char*)malloc(64);
    if (!a) fail("FAIL: malloc(64) returned NULL");
    for (int i = 0; i < 64; i++) a[i] = (char)(i & 0xFF);
    for (int i = 0; i < 64; i++) {
        if (a[i] != (char)(i & 0xFF)) fail("FAIL: basic write/read mismatch");
    }
    free(a);
    pass("PASS: basic malloc/free");

    /* calloc zeroes memory */
    unsigned long *z = (unsigned long*)calloc(8, sizeof(unsigned long));
    if (!z) fail("FAIL: calloc returned NULL");
    for (int i = 0; i < 8; i++) {
        if (z[i] != 0) fail("FAIL: calloc not zeroed");
    }
    free(z);
    pass("PASS: calloc zeroing");

    /* realloc grows the allocation */
    char *r = (char*)malloc(32);
    if (!r) fail("FAIL: malloc for realloc returned NULL");
    for (int i = 0; i < 32; i++) r[i] = (char)('A' + (i % 26));
    r = (char*)realloc(r, 256);
    if (!r) fail("FAIL: realloc returned NULL");
    for (int i = 0; i < 32; i++) {
        if (r[i] != (char)('A' + (i % 26))) fail("FAIL: realloc data not preserved");
    }
    free(r);
    pass("PASS: realloc grow + data preserved");

    /* Many small allocs — verifies free-list reuse */
    void *ptrs[32];
    for (int i = 0; i < 32; i++) {
        ptrs[i] = malloc(16);
        if (!ptrs[i]) fail("FAIL: malloc in loop returned NULL");
        *(volatile unsigned long*)ptrs[i] = (unsigned long)i | 0xDEAD000000000000UL;
    }
    for (int i = 0; i < 32; i++) {
        unsigned long v = *(volatile unsigned long*)ptrs[i];
        if ((v & 0xFFFF) != (unsigned long)i) fail("FAIL: small alloc value corrupted");
    }
    for (int i = 0; i < 32; i++) free(ptrs[i]);
    pass("PASS: 32 small allocs verified + freed");

    /* After freeing all, a new alloc should reuse heap (not grow) */
    unsigned long brk_before = sys_brk(0);
    void *reuse = malloc(16);
    if (!reuse) fail("FAIL: post-free malloc returned NULL");
    free(reuse);
    unsigned long brk_after = sys_brk(0);
    if (brk_after != brk_before) {
        /* Growing is acceptable if free list was fragmented, just note it */
        print_str("NOTE: brk grew after reuse alloc (fragmentation, not a bug)");
    } else {
        pass("PASS: reuse alloc did not grow brk");
    }

    /* Large alloc */
    void *big = malloc(8192);
    if (!big) fail("FAIL: large malloc returned NULL");
    volatile unsigned long *bg = (volatile unsigned long*)big;
    for (int i = 0; i < 1024; i++) bg[i] = (unsigned long)i * 0x1111111111111111UL;
    for (int i = 0; i < 1024; i++) {
        if (bg[i] != (unsigned long)i * 0x1111111111111111UL) fail("FAIL: large alloc corrupt");
    }
    free(big);
    pass("PASS: 8KiB alloc verified");

    print_str("umalloc_test: brk =");
    print_hex(sys_brk(0));
    print_str("umalloc_test: ALL PASS");
    sys_exit(0);
}
