/*
 * ush — FiFi OS user-space shell
 *
 * Reads lines from the keyboard, tokenises, forks a child, and execv's.
 * Programs are looked up by name; ".elf" is appended automatically.
 *
 * Builtins: exit, help
 */
#include "usys.h"
#include "ulibc.h"

#define LINEMAX  256
#define MAXARGS  8

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
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; sys_write("\b \b", 3); }
            continue;
        }
        if ((unsigned char)c < 32 || (unsigned char)c >= 128) continue;
        if (n + 1 >= cap) continue;
        buf[n++] = (char)c;
        sys_write(buf + n - 1, 1);
    }
}

/* ── tokenise ────────────────────────────────────────────────────────────── */

/* Split buf in-place on spaces. Fills toks[] (NULL-terminated). Returns ntok. */
static int ush_tokenise(char *buf, char *toks[], int maxtok) {
    int n = 0;
    char *p = buf;
    while (*p && n < maxtok) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        toks[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    toks[n] = (char*)0;
    return n;
}

/* ── exec helper ─────────────────────────────────────────────────────────── */

/* Try name as-is, then name + ".elf". argv[0] stays as the user typed it. */
static long ush_execv(const char *name, const char *const *argv) {
    long r = sys_execv(name, argv);
    if (r == 0) return 0;

    static char path[LINEMAX + 8];
    int i = 0;
    while (name[i] && i < LINEMAX - 1) { path[i] = name[i]; i++; }
    path[i++] = '.'; path[i++] = 'e'; path[i++] = 'l'; path[i++] = 'f';
    path[i] = '\0';
    return sys_execv(path, argv);
}

/* ── builtins ────────────────────────────────────────────────────────────── */

static int ush_builtin(int argc, char *argv[]) {
    if (argc == 0) return 0;
    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        int code = (argc > 1) ? (int)(argv[1][0] - '0') : 0;
        printf("bye\n");
        sys_exit(code);
    }
    if (strcmp(argv[0], "help") == 0) {
        printf("ush builtins: exit  quit  help\n");
        printf("Run any .elf from initrd/disk — .elf suffix is optional.\n");
        printf("Examples:  ucat motd.txt    uinfo    ucat /hello.txt\n");
        return 1;
    }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\nFiFi ush - user shell (tid %llu)\n",
           (unsigned long long)sys_gettid());
    printf("Type 'help' for help, 'exit' to quit.\n\n");

    static char line[LINEMAX];
    static char *toks[MAXARGS + 1];

    for (;;) {
        printf("$ ");
        int n = ush_readline(line, LINEMAX);
        if (n == 0) continue;

        int ntok = ush_tokenise(line, toks, MAXARGS);
        if (ntok == 0) continue;

        if (ush_builtin(ntok, toks)) continue;

        /* external command: fork → execv → waitpid */
        long child = sys_fork();
        if (child < 0) { printf("ush: fork failed\n"); continue; }

        if (child == 0) {
            /* child */
            long r = ush_execv(toks[0], (const char *const *)toks);
            if (r < 0) {
                printf("ush: not found: %s\n", toks[0]);
                sys_exit(127);
            }
            sys_exit(1); /* unreachable after exec */
        }

        /* parent */
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
