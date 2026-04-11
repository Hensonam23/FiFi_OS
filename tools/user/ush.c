/*
 * ush — FiFi OS user-space shell
 *
 * Reads lines from the keyboard, tokenises, forks a child, and execv's.
 * Programs are looked up by name; ".elf" is appended automatically.
 *
 * Builtins: exit, help, echo, cat, ls, rm, mkdir, stat, cp, mv, export, unset, env, source
 */
#include "usys.h"
#include "ulibc.h"

#define LINEMAX  256
#define MAXARGS  16
#define HIST_MAX 20

/* Special key codes from the keyboard driver (matches kernel/include/keyboard.h) */
#define KEY_UP   0x82
#define KEY_DOWN 0x83

/* ── command history ─────────────────────────────────────────────────────── */
static char g_hist_buf[HIST_MAX][LINEMAX];
static int  g_hist_n = 0;

/* ── environment variables ───────────────────────────────────────────────── */

#define ENV_MAX     24
#define ENV_KEY_MAX 32
#define ENV_VAL_MAX 128

static char g_env_keys[ENV_MAX][ENV_KEY_MAX];
static char g_env_vals[ENV_MAX][ENV_VAL_MAX];
static int  g_env_n = 0;

static const char *env_get(const char *key) {
    for (int i = 0; i < g_env_n; i++)
        if (strcmp(g_env_keys[i], key) == 0) return g_env_vals[i];
    return (char*)0;
}

static void env_set(const char *key, const char *val) {
    for (int i = 0; i < g_env_n; i++) {
        if (strcmp(g_env_keys[i], key) == 0) {
            int j = 0;
            while (val[j] && j < ENV_VAL_MAX - 1) { g_env_vals[i][j] = val[j]; j++; }
            g_env_vals[i][j] = '\0';
            return;
        }
    }
    if (g_env_n < ENV_MAX) {
        int j = 0;
        while (key[j] && j < ENV_KEY_MAX - 1) { g_env_keys[g_env_n][j] = key[j]; j++; }
        g_env_keys[g_env_n][j] = '\0';
        j = 0;
        while (val[j] && j < ENV_VAL_MAX - 1) { g_env_vals[g_env_n][j] = val[j]; j++; }
        g_env_vals[g_env_n][j] = '\0';
        g_env_n++;
    }
}

static void env_unset(const char *key) {
    for (int i = 0; i < g_env_n; i++) {
        if (strcmp(g_env_keys[i], key) == 0) {
            for (int j = i; j < g_env_n - 1; j++) {
                int k = 0;
                while ((g_env_keys[j][k] = g_env_keys[j+1][k])) k++;
                k = 0;
                while ((g_env_vals[j][k] = g_env_vals[j+1][k])) k++;
            }
            g_env_n--;
            return;
        }
    }
}

/* ── $VAR expansion ──────────────────────────────────────────────────────── */

/* Expand $VAR references in src into dst (cap bytes). */
static void ush_expand(char *dst, int cap, const char *src) {
    int di = 0;
    for (int si = 0; src[si] && di + 1 < cap; ) {
        if (src[si] == '$') {
            si++;
            char varname[ENV_KEY_MAX]; int vi = 0;
            while (src[si] && src[si] != '/' && src[si] != '$' && vi < ENV_KEY_MAX - 1)
                varname[vi++] = src[si++];
            varname[vi] = '\0';
            const char *v = env_get(varname);
            if (v) { while (*v && di + 1 < cap) dst[di++] = *v++; }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

/* Storage for expanded tokens (avoids stack allocation inside loops). */
static char g_exp_store[MAXARGS][LINEMAX];

/* ── readline ────────────────────────────────────────────────────────────── */

static int ush_readline(char *buf, int cap) {
    int n = 0;
    int hist_pos = -1;  /* -1 = live input, else index into g_hist_buf */
    for (;;) {
        int c = sys_getchar();
        if (c == '\r' || c == '\n') {
            sys_write("\n", 1);
            buf[n] = '\0';
            /* Save non-empty lines to history (skip exact duplicate of last entry) */
            if (n > 0) {
                int dup = (g_hist_n > 0 && strcmp(g_hist_buf[g_hist_n - 1], buf) == 0);
                if (!dup) {
                    if (g_hist_n < HIST_MAX) {
                        int i = 0;
                        while (buf[i] && i < LINEMAX - 1) { g_hist_buf[g_hist_n][i] = buf[i]; i++; }
                        g_hist_buf[g_hist_n][i] = '\0';
                        g_hist_n++;
                    } else {
                        /* Ring buffer: drop oldest, shift everything down */
                        for (int i = 0; i < HIST_MAX - 1; i++) {
                            int j = 0;
                            while ((g_hist_buf[i][j] = g_hist_buf[i+1][j])) j++;
                        }
                        int i = 0;
                        while (buf[i] && i < LINEMAX - 1) { g_hist_buf[HIST_MAX-1][i] = buf[i]; i++; }
                        g_hist_buf[HIST_MAX-1][i] = '\0';
                    }
                }
            }
            return n;
        }
        if (c == 3) {            /* Ctrl-C: cancel current line */
            sys_write("^C\n", 3);
            buf[0] = '\0';
            return 0;
        }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; sys_write("\b \b", 3); }
            continue;
        }
        /* History navigation */
        if (c == KEY_UP || c == KEY_DOWN) {
            int new_pos;
            if (c == KEY_UP) {
                if (g_hist_n == 0) continue;
                new_pos = (hist_pos < 0) ? g_hist_n - 1
                                         : (hist_pos > 0 ? hist_pos - 1 : 0);
            } else {
                new_pos = (hist_pos < 0) ? -1
                        : (hist_pos + 1 < g_hist_n ? hist_pos + 1 : -1);
            }
            /* Erase what's currently on screen */
            for (int i = 0; i < n; i++) sys_write("\b \b", 3);
            hist_pos = new_pos;
            if (hist_pos < 0) {
                n = 0;
                buf[0] = '\0';
            } else {
                n = 0;
                while (g_hist_buf[hist_pos][n] && n < cap - 1) {
                    buf[n] = g_hist_buf[hist_pos][n]; n++;
                }
                buf[n] = '\0';
                sys_write(buf, (uint64_t)n);
            }
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

/* Build a NULL-terminated envp array from the current shell environment.
 * Returns a pointer to a static envp[] ready to pass to sys_execve. */
#define USH_ENVSTR_MAX (ENV_KEY_MAX + 1 + ENV_VAL_MAX)
static char        g_envp_strs[ENV_MAX][USH_ENVSTR_MAX];
static const char *g_envp[ENV_MAX + 1];

static const char *const *ush_build_envp(void) {
    int n = 0;
    for (int i = 0; i < g_env_n && i < ENV_MAX; i++) {
        char *p = g_envp_strs[i];
        int j = 0;
        while (g_env_keys[i][j] && j < ENV_KEY_MAX - 1) *p++ = g_env_keys[i][j++];
        *p++ = '=';
        j = 0;
        while (g_env_vals[i][j] && j < ENV_VAL_MAX - 1) *p++ = g_env_vals[i][j++];
        *p = '\0';
        g_envp[n++] = g_envp_strs[i];
    }
    g_envp[n] = (char*)0;
    return g_envp;
}

/* Try name as-is, then name + ".elf". Passes current environment to child. */
static long ush_execv(const char *name, const char *const *argv) {
    const char *const *envp = ush_build_envp();
    long r = sys_execve(name, argv, envp);
    if (r == 0) return 0;

    static char path[LINEMAX + 8];
    int i = 0;
    while (name[i] && i < LINEMAX - 1) { path[i] = name[i]; i++; }
    path[i++] = '.'; path[i++] = 'e'; path[i++] = 'l'; path[i++] = 'f';
    path[i] = '\0';
    return sys_execve(path, argv, envp);
}

/* ── forward declaration (source builtin needs this) ─────────────────────── */
static int ush_exec_line(char *line);

/* ── builtins ────────────────────────────────────────────────────────────── */

static char ls_buf[4096];
static char cat_buf[64 * 1024];

static int ush_builtin(int argc, char *argv[]) {
    if (argc == 0) return 0;

    if (strcmp(argv[0], "help") == 0) {
        printf("builtins: exit quit help echo cat ls rm mkdir stat cp mv\n");
        printf("          cd pwd export unset env source\n");
        printf("Redirections: > (overwrite)  >> (append)  < (stdin)\n");
        printf("Pipes:        cmd1 | cmd2\n");
        printf("Variables:    export KEY=VAL  echo $KEY  unset KEY\n");
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

    if (strcmp(argv[0], "rm") == 0) {
        if (argc < 2) { printf("usage: rm <file>\n"); return 1; }
        for (int i = 1; i < argc; i++) {
            long r = sys_unlink(argv[i]);
            if (r < 0) printf("rm: cannot remove: %s\n", argv[i]);
        }
        return 1;
    }

    if (strcmp(argv[0], "mkdir") == 0) {
        if (argc < 2) { printf("usage: mkdir <dir>\n"); return 1; }
        for (int i = 1; i < argc; i++) {
            long r = sys_mkdir(argv[i]);
            if (r < 0) printf("mkdir: cannot create: %s\n", argv[i]);
        }
        return 1;
    }

    if (strcmp(argv[0], "stat") == 0) {
        if (argc < 2) { printf("usage: stat <file>\n"); return 1; }
        long sz = sys_filesize(argv[1]);
        if (sz < 0) printf("stat: not found: %s\n", argv[1]);
        else        printf("%s: %ld byte%s\n", argv[1], sz, sz == 1 ? "" : "s");
        return 1;
    }

    if (strcmp(argv[0], "cp") == 0) {
        if (argc < 3) { printf("usage: cp <src> <dst>\n"); return 1; }
        long n = sys_readfile(argv[1], cat_buf, (uint64_t)(sizeof(cat_buf) - 1));
        if (n < 0) { printf("cp: cannot read: %s\n", argv[1]); return 1; }
        long fd = sys_creat(argv[2]);
        if (fd < 0) { printf("cp: cannot create: %s\n", argv[2]); return 1; }
        sys_fdwrite((int)fd, cat_buf, (uint64_t)n);
        sys_close((int)fd);
        return 1;
    }

    if (strcmp(argv[0], "mv") == 0) {
        if (argc < 3) { printf("usage: mv <src> <dst>\n"); return 1; }
        long n = sys_readfile(argv[1], cat_buf, (uint64_t)(sizeof(cat_buf) - 1));
        if (n < 0) { printf("mv: cannot read: %s\n", argv[1]); return 1; }
        long fd = sys_creat(argv[2]);
        if (fd < 0) { printf("mv: cannot create: %s\n", argv[2]); return 1; }
        sys_fdwrite((int)fd, cat_buf, (uint64_t)n);
        sys_close((int)fd);
        sys_unlink(argv[1]);
        return 1;
    }

    if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            /* print all */
            for (int i = 0; i < g_env_n; i++)
                printf("%s=%s\n", g_env_keys[i], g_env_vals[i]);
            return 1;
        }
        for (int i = 1; i < argc; i++) {
            /* find '=' */
            int ei = 0;
            while (argv[i][ei] && argv[i][ei] != '=') ei++;
            if (argv[i][ei] == '=') {
                argv[i][ei] = '\0';
                env_set(argv[i], argv[i] + ei + 1);
                argv[i][ei] = '=';
            } else {
                /* export NAME with no value: print it */
                const char *v = env_get(argv[i]);
                if (v) printf("%s=%s\n", argv[i], v);
            }
        }
        return 1;
    }

    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; i < argc; i++) env_unset(argv[i]);
        return 1;
    }

    if (strcmp(argv[0], "env") == 0) {
        for (int i = 0; i < g_env_n; i++)
            printf("%s=%s\n", g_env_keys[i], g_env_vals[i]);
        return 1;
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argc < 2) { printf("usage: source <script>\n"); return 1; }
        long n = sys_readfile(argv[1], cat_buf, (uint64_t)(sizeof(cat_buf) - 1));
        if (n < 0) { printf("source: not found: %s\n", argv[1]); return 1; }
        cat_buf[n] = '\0';
        /* Execute each line of the script */
        char *p = cat_buf;
        while (*p) {
            char *nl = p;
            while (*nl && *nl != '\n') nl++;
            char saved = *nl;
            *nl = '\0';
            /* Skip blank lines and comments */
            char *t = p;
            while (*t == ' ' || *t == '\t') t++;
            if (*t && *t != '#') ush_exec_line(p);
            *nl = saved;
            p = (*nl) ? nl + 1 : nl;
        }
        return 1;
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *target = (argc >= 2) ? argv[1] : "/";
        if (sys_chdir(target) < 0)
            printf("cd: not a directory: %s\n", target);
        return 1;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[256];
        if (sys_getcwd(cwd, sizeof(cwd)) < 0) printf("/\n");
        else printf("%s\n", cwd);
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

/* ── redirection helpers ─────────────────────────────────────────────────── */

/*
 * Scan toks[0..ntok-1] for ">", ">>", and "<" redirection operators.
 * Removes them (and their filename arguments) from the token list in-place.
 * Sets *out_file / *in_file to the corresponding filename, or NULL if absent.
 * Sets *append to true if ">>" was used.
 * Updates *ntok_p to the new (reduced) token count.
 */
static void ush_extract_redirects(char *toks[], int *ntok_p,
                                   char **out_file, char **in_file,
                                   int *append) {
    *out_file = (char*)0;
    *in_file  = (char*)0;
    *append   = 0;
    int n = *ntok_p;
    int w = 0;
    for (int i = 0; i < n; ) {
        if (toks[i][0] == '>' && toks[i][1] == '>' && toks[i][2] == '\0' && i + 1 < n) {
            *out_file = toks[i + 1];
            *append   = 1;
            i += 2;
        } else if (toks[i][0] == '>' && toks[i][1] == '\0' && i + 1 < n) {
            *out_file = toks[i + 1];
            *append   = 0;
            i += 2;
        } else if (toks[i][0] == '<' && toks[i][1] == '\0' && i + 1 < n) {
            *in_file = toks[i + 1];
            i += 2;
        } else {
            toks[w++] = toks[i++];
        }
    }
    toks[w] = (char*)0;
    *ntok_p  = w;
}

/* Apply stdin/stdout redirections in a child process (call before exec). */
static void ush_apply_redirects(const char *out_file, const char *in_file, int append) {
    if (in_file) {
        long fd = sys_open(in_file);
        if (fd >= 0) { sys_dup2((int)fd, 0); sys_close((int)fd); }
    }
    if (out_file) {
        long fd = append ? sys_openw(out_file) : sys_creat(out_file);
        if (fd >= 0) { sys_dup2((int)fd, 1); sys_close((int)fd); }
    }
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
    /* File redirections apply after pipe wiring (pipe takes precedence for its end) */

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

/* ── execute one line ────────────────────────────────────────────────────── */

/*
 * Execute a single command line.  Used by both the interactive loop and
 * the 'source' builtin.  line[] is modified in-place by the tokeniser.
 * Returns 0 normally, 1 if the shell should exit.
 */
static int ush_exec_line(char *line) {
    static char *toks[MAXARGS + 1];

    int ntok = ush_tokenise(line, toks, MAXARGS);
    if (ntok == 0) return 0;

    /* ── $VAR expansion ── */
    for (int i = 0; i < ntok; i++) {
        if (strchr(toks[i], '$')) {
            ush_expand(g_exp_store[i], LINEMAX, toks[i]);
            toks[i] = g_exp_store[i];
        }
    }

    /* ── handle KEY=VALUE assignments at the start of a line ── */
    if (ntok == 1 && strchr(toks[0], '=')) {
        int ei = 0;
        while (toks[0][ei] && toks[0][ei] != '=') ei++;
        if (toks[0][ei] == '=') {
            toks[0][ei] = '\0';
            env_set(toks[0], toks[0] + ei + 1);
            toks[0][ei] = '=';
            return 0;
        }
    }

    /* ── extract redirections (>, >>, <) before anything else ── */
    char *redir_out = (char*)0;
    char *redir_in  = (char*)0;
    int   redir_app = 0;
    ush_extract_redirects(toks, &ntok, &redir_out, &redir_in, &redir_app);
    if (ntok == 0) return 0;

    /* ── exit/quit: handle in-process so source scripts can exit the shell ── */
    if (strcmp(toks[0], "exit") == 0 || strcmp(toks[0], "quit") == 0) {
        int code = (ntok > 1) ? (int)(toks[1][0] - '0') : 0;
        printf("bye\n");
        sys_exit(code);
    }

    /* ── pipe: cmd1 | cmd2 ── */
    int pipe_idx = find_pipe(toks, ntok);
    if (pipe_idx >= 0) {
        toks[pipe_idx] = (char*)0;
        char **left_toks  = toks;
        int    left_ntok  = pipe_idx;
        char **right_toks = toks + pipe_idx + 1;
        int    right_ntok = ntok - pipe_idx - 1;

        if (left_ntok == 0 || right_ntok == 0) {
            printf("ush: syntax error near '|'\n");
            return 0;
        }

        int pipefd[2];
        if (sys_pipe(pipefd) < 0) { printf("ush: pipe failed\n"); return 0; }

        long c1 = run_piped(left_toks,  left_ntok,
                             -1,        pipefd[1],
                             pipefd[0], -1);
        if (c1 < 0) { printf("ush: fork failed\n"); sys_close(pipefd[0]); sys_close(pipefd[1]); return 0; }

        long c2 = run_piped(right_toks, right_ntok,
                             pipefd[0], -1,
                             pipefd[1], -1);
        if (c2 < 0) { printf("ush: fork failed\n"); sys_close(pipefd[0]); sys_close(pipefd[1]); return 0; }

        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
        int code = 0;
        sys_waitpid((unsigned long)c1, &code);
        sys_waitpid((unsigned long)c2, &code);
        if (code != 0 && code != 127) printf("[exit %d]\n", code);
        return 0;
    }

    /* ── no pipe: regular command or builtin ── */
    if (!redir_out && !redir_in && ush_builtin(ntok, toks)) return 0;

    long child = sys_fork();
    if (child < 0) { printf("ush: fork failed\n"); return 0; }

    if (child == 0) {
        ush_apply_redirects(redir_out, redir_in, redir_app);
        if (ush_builtin(ntok, toks)) sys_exit(0);
        long r = ush_execv(toks[0], (const char *const *)toks);
        if (r < 0) { printf("ush: not found: %s\n", toks[0]); sys_exit(127); }
        sys_exit(1);
    }

    int code = 0;
    long reaped = sys_waitpid((unsigned long)child, &code);
    if (reaped < 0) {
        printf("ush: waitpid failed\n");
    } else if (code == 127) {
        /* "not found" already printed */
    } else if (code == 130) {
        /* Ctrl-C */
    } else if (code != 0) {
        printf("[exit %d]\n", code);
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
    static char cwd_buf[256];

    for (;;) {
        if (sys_getcwd(cwd_buf, sizeof(cwd_buf)) < 0) {
            cwd_buf[0] = '/'; cwd_buf[1] = '\0';
        }
        printf("fifi:%s$ ", cwd_buf);
        int n = ush_readline(line, LINEMAX);
        if (n == 0) continue;
        ush_exec_line(line);
    }
}
