#pragma once
#include <stdint.h>

/*
  Stable syscall numbers (shared by kernel and userland).
  Keep these explicit so they never drift.
*/
enum {
    SYS_NOP       = 0,
    SYS_LOG       = 1,
    SYS_UPTIME    = 2,
    SYS_YIELD     = 3,
    SYS_EXIT      = 4,
    SYS_SLEEP_MS  = 5,
    SYS_GETTID    = 6,
    SYS_WRITE     = 7,
    SYS_READFILE  = 11,
    SYS_OPEN      = 8,
    SYS_READ      = 9,
    SYS_CLOSE     = 10,
};
