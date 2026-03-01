#pragma once
#include <stdint.h>
#include <stddef.h>

/*
  Selectors (per your CURRENT working layout)
  kernel cs = 0x28, kernel ds = 0x30
  user   ds = 0x3B, user   cs = 0x43
  tss        = 0x48
*/
#define FIFI_KERNEL_CS 0x28
#define FIFI_KERNEL_DS 0x30
#define FIFI_USER_DS   0x3B
#define FIFI_USER_CS   0x43
#define FIFI_TSS_SEL   0x48

// Top of canonical lower-half user VA (page aligned)
#define FIFI_USER_TOP           0x00007ffffffff000ULL

// Temporary: userdemo uses a fixed stack top today.
#define FIFI_USERDEMO_STACK_TOP  0x0000000000500000ULL

// A 4K trampoline page we ALWAYS map for user tasks.
// On user exception, we redirect RIP here so it can do SYS_EXIT safely.
#define FIFI_USER_TRAMPOLINE_VA (FIFI_USER_TOP - 0x1000ULL)

// User stack lives directly below the trampoline page.
#define FIFI_USER_STACK_PAGES   8ULL
#define FIFI_USER_STACK_TOP     (FIFI_USER_TRAMPOLINE_VA)
#define FIFI_USER_STACK_BASE    (FIFI_USER_STACK_TOP - (FIFI_USER_STACK_PAGES * 0x1000ULL))

// The trampoline code is tiny:
//   int $0x80
//   hlt
//   jmp $
static const uint8_t FIFI_USER_TRAMPOLINE_CODE[4] = {
  0xCD, 0x80, 0xEB, 0xFC
};

static inline int fifi_cs_is_user(uint64_t cs) {
  return (int)(cs & 0x3ULL) == 3;
}
