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
    SYS_BRK       = 14,
    SYS_WAITPID   = 15,
    SYS_GETCHAR   = 16,
    SYS_LISTFILES = 17,
    SYS_PIPE      = 18,
    SYS_DUP2      = 19,
    SYS_CREAT     = 20,
    SYS_UNLINK    = 21,  /* delete a file */
    SYS_OPENW     = 22,  /* open for append-write (RAMW fd pre-loaded with existing content) */
    SYS_FILESIZE  = 23,  /* return file size in bytes, or -1 */
    SYS_MKDIR     = 24,  /* create a directory */
    SYS_GETCWD    = 25,  /* get current working directory */
    SYS_CHDIR     = 26,  /* change working directory */

    SYS_KILL      = 27,  /* kill(tid, sig): send signal to thread */
    SYS_SIGNAL    = 28,  /* signal(sig, handler): install user handler */
    SYS_MMAP      = 29,  /* mmap(addr, len, prot): map anon pages */
    SYS_MUNMAP    = 30,  /* munmap(addr, len): unmap pages */
    SYS_SETPGID   = 31,  /* setpgid(tid, pgid): set process group */
    SYS_LISTDIR   = 32,  /* listdir(path, buf, cap): list directory entries */
    SYS_WAITFLAGS = 33,  /* waitpid with WUNTRACED / WNOHANG flags */
};
