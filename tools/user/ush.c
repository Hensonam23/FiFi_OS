/*
 * ush — FiFi OS user-space shell
 *
 * Reads lines from the keyboard, forks a child, and exec's the named ELF.
 * Programs are looked up as "<name>.elf" in the VFS (initrd or ext2 disk).
 *
 * Builtins: exit, help
 */
#include "usys.h"
#include "ulibc.h"

#define LINEMAX 256

/* ── readline ────────────────────────────────────────────────────────────── */

static int ush_readline(char *buf, int cap) {
    int n = 0;
    for (;;) {
        int c = sys_getchar();

        if (c == '\r' || c == '\n') {
            sys_write("\n", 1);
            buf[n] = '\0';
            return n;
        }

        /* backspace / DEL */
        if (c == '\b' || c == 127) {
            if (n > 0) {
                n--;
                sys_write("\b \b", 3);
            }
            continue;
        }

        /* ignore other control characters */
        if ((unsigned char)c < 32 || (unsigned char)c >= 128)
            continue;

        if (n + 1 >= cap)
            continue;   /* line too long — silently drop */

        buf[n++] = (char)c;
        sys_write(buf + n - 1, 1); /* echo */
    }
}

/* ── trim & tokenise ─────────────────────────────────────────────────────── */

/* Trim leading spaces, return pointer into buf. */
static char *ush_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ── builtins ────────────────────────────────────────────────────────────── */

static int ush_builtin(const char *cmd) {
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        printf("bye\n");
        sys_exit(0);
    }
    if (strcmp(cmd, "help") == 0) {
        printf("ush builtins: exit  help\n");
        printf("Run any .elf from initrd/disk by name (without .elf suffix).\n");
        printf("Examples:  ucat motd.txt    uinfo    uwait\n");
        return 1;
    }
    return 0; /* not a builtin */
}

/* ── exec helper ─────────────────────────────────────────────────────────── */

/*
 * Try to exec 'name' as a VFS path.  We try:
 *   1. name as-is  (in case user typed "ucat.elf" explicitly)
 *   2. name + ".elf"
 * Never returns on success (exec replaces us).
 * Returns -1 if both attempts fail.
 */
static long ush_exec(const char *name) {
    long r = sys_exec(name);
    if (r == 0) return 0; /* succeeded — shouldn't reach here */

    /* try appending .elf */
    static char path[LINEMAX + 8];
    int i = 0;
    while (name[i] && i < LINEMAX - 1) { path[i] = name[i]; i++; }
    path[i++] = '.'; path[i++] = 'e'; path[i++] = 'l'; path[i++] = 'f';
    path[i] = '\0';

    r = sys_exec(path);
    return r;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\nFiFi ush - user shell (tid %llu)\n",
           (unsigned long long)sys_gettid());
    printf("Type 'help' for help, 'exit' to quit.\n\n");

    static char line[LINEMAX];

    for (;;) {
        printf("$ ");

        int n = ush_readline(line, LINEMAX);
        if (n == 0) continue;

        char *cmd = ush_trim(line);
        if (*cmd == '\0') continue;

        /* builtins */
        if (ush_builtin(cmd)) continue;

        /* external command: fork + exec + waitpid */
        long child = sys_fork();
        if (child < 0) {
            printf("ush: fork failed\n");
            continue;
        }

        if (child == 0) {
            /* child process */
            long r = ush_exec(cmd);
            if (r < 0) {
                printf("ush: not found: %s\n", cmd);
                sys_exit(127);
            }
            /* exec replaced us; unreachable */
            sys_exit(1);
        }

        /* parent: wait for child */
        int code = 0;
        long reaped = sys_waitpid((unsigned long)child, &code);
        if (reaped < 0) {
            printf("ush: waitpid failed\n");
        } else if (code == 127) {
            /* "not found" already printed by child */
        } else if (code != 0) {
            printf("[exit %d]\n", code);
        }
    }
}
