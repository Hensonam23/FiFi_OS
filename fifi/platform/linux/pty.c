#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>

/* console_putc declared here — defined in console.c, compiled into same binary */
void console_putc(char c);

static int   g_pty_master = -1;
static pid_t g_pty_child  = -1;

/* Translate FIFI_KEY_* extended codes (>= 0x80) to VT100/ANSI escape sequences */
static int fifi_to_ansi(uint8_t c, char *buf) {
    switch (c) {
    /* Arrow keys */
    case 0x80: memcpy(buf, "\033[D", 3); return 3;
    case 0x81: memcpy(buf, "\033[C", 3); return 3;
    case 0x82: memcpy(buf, "\033[A", 3); return 3;
    case 0x83: memcpy(buf, "\033[B", 3); return 3;
    /* Navigation */
    case 0x84: memcpy(buf, "\033[3~", 4); return 4;  /* Delete */
    case 0x85: memcpy(buf, "\033[H",  3); return 3;  /* Home */
    case 0x86: memcpy(buf, "\033[F",  3); return 3;  /* End */
    case 0x87: memcpy(buf, "\033[5~", 4); return 4;  /* PgUp */
    case 0x88: memcpy(buf, "\033[6~", 4); return 4;  /* PgDn */
    /* Alt+Tab — send ESC+Tab */
    case 0x89: memcpy(buf, "\033\t",  2); return 2;
    /* F1-F12 */
    case 0x8A: memcpy(buf, "\033OP",   3); return 3;
    case 0x8B: memcpy(buf, "\033OQ",   3); return 3;
    case 0x8C: memcpy(buf, "\033OR",   3); return 3;
    case 0x8D: memcpy(buf, "\033OS",   3); return 3;
    case 0x8E: memcpy(buf, "\033[15~", 5); return 5;
    case 0x8F: memcpy(buf, "\033[17~", 5); return 5;
    case 0x90: memcpy(buf, "\033[18~", 5); return 5;
    case 0x91: memcpy(buf, "\033[19~", 5); return 5;
    case 0x92: memcpy(buf, "\033[20~", 5); return 5;
    case 0x93: memcpy(buf, "\033[21~", 5); return 5;
    case 0x94: memcpy(buf, "\033[23~", 5); return 5;
    case 0x95: memcpy(buf, "\033[24~", 5); return 5;
    default:
        buf[0] = (char)c;
        return 1;
    }
}

static void pty_sigchld(int sig) {
    (void)sig;
    int st;
    if (g_pty_child > 0 && waitpid(g_pty_child, &st, WNOHANG) == g_pty_child)
        g_pty_child = -1;
}

void pty_init(void) {
    signal(SIGCHLD, pty_sigchld);
    signal(SIGPIPE, SIG_IGN);

    g_pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (g_pty_master < 0) {
        fprintf(stderr, "[pty] posix_openpt: %s\n", strerror(errno));
        return;
    }
    if (grantpt(g_pty_master) < 0 || unlockpt(g_pty_master) < 0) {
        fprintf(stderr, "[pty] grant/unlock: %s\n", strerror(errno));
        close(g_pty_master); g_pty_master = -1; return;
    }

    /* Initial PTY size: conservative estimate for 1024x768 terminal window */
    struct winsize ws = { .ws_row = 38, .ws_col = 111 };
    ioctl(g_pty_master, TIOCSWINSZ, &ws);

    char *slave_name = ptsname(g_pty_master);
    if (!slave_name) {
        fprintf(stderr, "[pty] ptsname failed\n");
        close(g_pty_master); g_pty_master = -1; return;
    }

    int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        fprintf(stderr, "[pty] open slave: %s\n", strerror(errno));
        close(g_pty_master); g_pty_master = -1; return;
    }

    g_pty_child = fork();
    if (g_pty_child < 0) {
        fprintf(stderr, "[pty] fork: %s\n", strerror(errno));
        close(slave_fd); close(g_pty_master); g_pty_master = -1; return;
    }

    if (g_pty_child == 0) {
        /* Child: become session leader, set controlling terminal */
        close(g_pty_master);
        if (setsid() < 0) _exit(1);
        ioctl(slave_fd, TIOCSCTTY, 0);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);

        setenv("TERM",  "linux",      1);
        setenv("HOME",  "/root",      1);
        setenv("PATH",  "/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("USER",  "root",       1);
        setenv("SHELL", "/bin/sh",    1);
        setenv("PS1",   "\\w # ",     1);

        /* Try ush first (FiFi shell), then busybox sh, then sh */
        execl("/bin/ush", "-ush", NULL);
        execl("/bin/sh",  "-sh",  NULL);
        execl("/bin/busybox", "sh", NULL);
        _exit(1);
    }

    close(slave_fd);

    /* Set master non-blocking */
    int fl = fcntl(g_pty_master, F_GETFL, 0);
    fcntl(g_pty_master, F_SETFL, fl | O_NONBLOCK);

    fprintf(stderr, "[pty] shell pid=%d tty=%s cols=%d rows=%d\n",
            (int)g_pty_child, slave_name, ws.ws_col, ws.ws_row);
}

int pty_master_fd(void) { return g_pty_master; }

/* Read all available PTY output and push each byte through console_putc() */
void pty_poll_output(void) {
    if (g_pty_master < 0) return;
    char buf[4096];
    ssize_t n;
    while ((n = read(g_pty_master, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++)
            console_putc(buf[i]);
    }
}

/* Write a single key to the PTY, translating FiFi extended codes to ANSI */
void pty_write_input(uint8_t c) {
    if (g_pty_master < 0) return;
    char ansi[8];
    int len = fifi_to_ansi(c, ansi);
    const char *p = ansi;
    while (len > 0) {
        ssize_t w = write(g_pty_master, p, (size_t)len);
        if (w <= 0) break;
        p += w; len -= (int)w;
    }
}

/* Update PTY window size (call when terminal window is resized) */
void pty_set_winsize(uint16_t cols, uint16_t rows) {
    if (g_pty_master < 0) return;
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    ioctl(g_pty_master, TIOCSWINSZ, &ws);
}
