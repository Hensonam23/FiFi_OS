#pragma once
#include <stdint.h>

/*
  Stable syscall numbers (shared by kernel + userland).
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

    // File syscalls (make sure these do NOT collide)
    SYS_OPEN      = 8,
    SYS_CLOSE     = 9,
    SYS_READ      = 10,
    SYS_READFILE  = 11,
    SYS_EXEC      = 12,
    SYS_FORK      = 13,
};
