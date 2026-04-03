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

static char ls_buf[4096];
static char cat_buf[64 * 1024];

static int ush_builtin(int argc, char *argv[]) {
    if (argc == 0) return 0;

    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        int code = (argc > 1) ? (int)(argv[1][0] - '0') : 0;
        printf("bye\n");
        sys_exit(code);
    }

    if (strcmp(argv[0], "help") == 0) {
        printf("builtins: exit quit help echo cat ls\n");
        printf("Run any .elf — .elf suffix optional. Examples:\n");
        printf("  ucat motd.txt    uinfo    uwait    uls\n");
        return 1;
    }

    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) sys_write(" ", 1);
            sys_write(argv[i], (uint64_t)strlen(argv[i]));
        }
        sys_write("\n", 1);
        return 1;
    }

    if (strcmp(argv[0], "ls") == 0) {
        long n = sys_listfiles(ls_buf, sizeof(ls_buf));
        if (n > 0) sys_write(ls_buf, (uint64_t)n);
        return 1;
    }

    if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) { printf("usage: cat <file>\n"); return 1; }
        long n = sys_readfile(argv[1], cat_buf, (uint64_t)(sizeof(cat_buf) - 1));
        if (n < 0) { printf("cat: not found: %s\n", argv[1]); return 1; }
        sys_write(cat_buf, (uint64_t)n);
        if (n > 0 && cat_buf[n - 1] != '\n') sys_write("\n", 1);
        return 1;
    }

    return 0;
}

/* ── pipe helper ─────────────────────────────────────────────────────────── */

/*
 * Find a pipe '|' token in toks[0..ntok-1].
 * Returns the index of '|', or -1 if none.
 */
static int find_pipe(char *toks[], int ntok) {
    for (int i = 0; i < ntok; i++) {
        if (toks[i][0] == '|' && toks[i][1] == '\0') return i;
    }
    return -1;
}

/*
 * Run a single command (toks[0..ntok-1]) in a child.
 * Before exec:
 *   - close_read  ≥ 0  → close that fd
 *   - close_write ≥ 0  → close that fd
 *   - dup_read    ≥ 0  → dup2(dup_read, 0)   (stdin from pipe)
 *   - dup_write   ≥ 0  → dup2(dup_write, 1)  (stdout to pipe)
 * Returns child TID, or -1 on fork failure.
 */
static long run_piped(char *toks[], int ntok,
                      int dup_stdin,  int dup_stdout,
                      int close_a,    int close_b) {
    long child = sys_fork();
    if (child < 0) return -1;
    if (child != 0) return child;  /* parent — return child TID */

    /* child */
    if (close_a >= 0) sys_close(close_a);
    if (close_b >= 0) sys_close(close_b);
    if (dup_stdin  >= 0) { sys_dup2(dup_stdin,  0); sys_close(dup_stdin); }
    if (dup_stdout >= 0) { sys_dup2(dup_stdout, 1); sys_close(dup_stdout); }

    /* Try builtin first (rare for piped cmds, but handle echo etc.) */
    if (ush_builtin(ntok, toks)) sys_exit(0);

    long r = ush_execv(toks[0], (const char *const *)toks);
    if (r < 0) {
        printf("ush: not found: %s\n", toks[0]);
        sys_exit(127);
    }
    sys_exit(1);  /* unreachable */
    return -1;    /* silence compiler */
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

        /* ── pipe: cmd1 | cmd2 ── */
        int pipe_idx = find_pipe(toks, ntok);
        if (pipe_idx >= 0) {
            /* Split into left and right at '|' */
            toks[pipe_idx] = (char*)0;
            char **left_toks  = toks;
            int    left_ntok  = pipe_idx;
            char **right_toks = toks + pipe_idx + 1;
            int    right_ntok = ntok - pipe_idx - 1;

            if (left_ntok == 0 || right_ntok == 0) {
                printf("ush: syntax error near '|'\n");
                continue;
            }

            int pipefd[2];
            if (sys_pipe(pipefd) < 0) {
                printf("ush: pipe failed\n");
                continue;
            }

            /* left child: stdout → pipe write end; close read end */
            long c1 = run_piped(left_toks,  left_ntok,
                                 -1,        pipefd[1],   /* dup stdout = write end */
                                 pipefd[0], -1);          /* close read end in child */
            if (c1 < 0) { printf("ush: fork failed\n"); sys_close(pipefd[0]); sys_close(pipefd[1]); continue; }

            /* right child: stdin → pipe read end; close write end */
            long c2 = run_piped(right_toks, right_ntok,
                                 pipefd[0], -1,           /* dup stdin = read end */
                                 pipefd[1], -1);           /* close write end in child */
            if (c2 < 0) { printf("ush: fork failed\n"); sys_close(pipefd[0]); sys_close(pipefd[1]); continue; }

            /* parent: close both ends then wait */
            sys_close(pipefd[0]);
            sys_close(pipefd[1]);

            int code = 0;
            sys_waitpid((unsigned long)c1, &code);
            sys_waitpid((unsigned long)c2, &code);
            if (code != 0 && code != 127) printf("[exit %d]\n", code);
            continue;
        }

        /* ── no pipe: regular command ── */
        if (ush_builtin(ntok, toks)) continue;

        long child = sys_fork();
        if (child < 0) { printf("ush: fork failed\n"); continue; }

        if (child == 0) {
            long r = ush_execv(toks[0], (const char *const *)toks);
            if (r < 0) {
                printf("ush: not found: %s\n", toks[0]);
                sys_exit(127);
            }
            sys_exit(1);
        }

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
