/* FiFi Terminal — standalone IPC process.
 * Spawns a PTY shell (/bin/sh) and provides a VT100 terminal window.
 * Multiple instances can run simultaneously, each with its own shell.
 * Build: gcc -O2 -static -o fifi-terminal terminal.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

/* ── IPC protocol ────────────────────────────────────────────────────────── */
#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_TITLE    0x03u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_INVALIDATE   0x15u
#define IPC_WIN_RESIZE   0x1Bu

/* ── Window / terminal geometry ─────────────────────────────────────────── */
#define WIN_W     640
#define WIN_H     400
#define TITLE_H   24     /* compositor title bar */
#define PAD       4

/* ── Font (PSF1) ─────────────────────────────────────────────────────────── */
#define PSF1_MAGIC 0x0436u
typedef struct { uint16_t magic; uint8_t mode; uint8_t charsize; } Psf1Hdr;

static uint8_t *g_glyph   = NULL;
static int      g_glyph_h = 16;
static int      g_glyph_w = 8;
static int      g_n_glyph = 256;

static bool font_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Psf1Hdr hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) || hdr.magic != PSF1_MAGIC) {
        close(fd); return false;
    }
    g_glyph_h = hdr.charsize;
    g_n_glyph = (hdr.mode & 1) ? 512 : 256;
    int total  = g_n_glyph * g_glyph_h;
    g_glyph    = malloc((size_t)total);
    if (!g_glyph || read(fd, g_glyph, total) < total) {
        free(g_glyph); g_glyph = NULL; close(fd); return false;
    }
    close(fd);
    return true;
}

/* ── Terminal state ──────────────────────────────────────────────────────── */
#define COLS 80
#define ROWS 23   /* visible rows (WIN_H - TITLE_H - 2*PAD) / glyph_h */

typedef struct { uint8_t ch; uint32_t fg; uint32_t bg; } Cell;

static Cell    g_cells[ROWS][COLS];
static int     g_cx = 0, g_cy = 0;         /* cursor col, row */
static uint32_t g_fg = 0xFFd8e8f8u;
static uint32_t g_bg = 0xFF0e1418u;
static bool    g_cursor_vis = true;
static bool    g_dirty = true;
static int     g_pty_master = -1;
static pid_t   g_child_pid  = -1;

/* ESC sequence parser */
static bool  g_esc        = false;
static bool  g_esc_bracket = false;
static char  g_esc_buf[32];
static int   g_esc_len = 0;

static void cell_clear_all(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_cells[r][c] = (Cell){ ' ', g_fg, g_bg };
}

static void scroll_up(void) {
    memmove(&g_cells[0], &g_cells[1], sizeof(Cell) * COLS * (ROWS - 1));
    for (int c = 0; c < COLS; c++)
        g_cells[ROWS - 1][c] = (Cell){ ' ', g_fg, g_bg };
}

static void newline(void) {
    g_cx = 0;
    g_cy++;
    if (g_cy >= ROWS) { g_cy = ROWS - 1; scroll_up(); }
}

/* Map ANSI color index (0-7) to 32-bit ARGB */
static uint32_t ansi_color(int idx, bool bright) {
    static const uint32_t pal[8] = {
        0xFF0e1418u, 0xFF993333u, 0xFF33993eu, 0xFF999933u,
        0xFF336699u, 0xFF993399u, 0xFF339999u, 0xFFd8e8f8u,
    };
    static const uint32_t bright_pal[8] = {
        0xFF506070u, 0xFFcc4444u, 0xFF44cc44u, 0xFFcccc44u,
        0xFF4488ccu, 0xFFcc44ccu, 0xFF44ccccu, 0xFFf8f8f8u,
    };
    if (idx < 0 || idx > 7) return 0xFFd8e8f8u;
    return bright ? bright_pal[idx] : pal[idx];
}

/* Process a fully-buffered ESC[...m or other sequence */
static void handle_esc_seq(void) {
    if (!g_esc_bracket || g_esc_len == 0) {
        g_esc = g_esc_bracket = false; g_esc_len = 0; return;
    }
    char cmd = g_esc_buf[g_esc_len - 1];
    g_esc_buf[g_esc_len - 1] = '\0';  /* nul-terminate params */

    if (cmd == 'm') {
        /* Color / attribute reset: parse semicolon-separated params */
        char *p = g_esc_buf;
        while (*p) {
            int n = atoi(p);
            if (n == 0)                 { g_fg = ansi_color(7, false); g_bg = ansi_color(0, false); }
            else if (n == 1)            { /* bold — use bright fg */ }
            else if (n >= 30 && n <= 37) g_fg = ansi_color(n - 30, false);
            else if (n >= 90 && n <= 97) g_fg = ansi_color(n - 90, true);
            else if (n >= 40 && n <= 47) g_bg = ansi_color(n - 40, false);
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
        }
    } else if (cmd == 'J') {
        int n = atoi(g_esc_buf);
        if (n == 2) { cell_clear_all(); g_cx = 0; g_cy = 0; }
        else if (n == 0) {  /* clear to end of screen */
            for (int c = g_cx; c < COLS; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
            for (int r = g_cy + 1; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    g_cells[r][c] = (Cell){ ' ', g_fg, g_bg };
        }
    } else if (cmd == 'K') {
        int n = atoi(g_esc_buf);
        if (n == 0) for (int c = g_cx; c < COLS; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
        else if (n == 1) for (int c = 0; c <= g_cx; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
        else if (n == 2) for (int c = 0; c < COLS; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
    } else if (cmd == 'H' || cmd == 'f') {
        /* Cursor position ESC[row;colH */
        int row = 0, col = 0;
        char *sc = strchr(g_esc_buf, ';');
        row = atoi(g_esc_buf);
        if (sc) col = atoi(sc + 1);
        g_cx = (col > 1 ? col - 1 : 0);
        g_cy = (row > 1 ? row - 1 : 0);
        if (g_cx >= COLS) g_cx = COLS - 1;
        if (g_cy >= ROWS) g_cy = ROWS - 1;
    } else if (cmd == 'A') { int n = atoi(g_esc_buf); g_cy -= n ? n : 1; if (g_cy < 0) g_cy = 0; }
    else if (cmd == 'B') { int n = atoi(g_esc_buf); g_cy += n ? n : 1; if (g_cy >= ROWS) g_cy = ROWS-1; }
    else if (cmd == 'C') { int n = atoi(g_esc_buf); g_cx += n ? n : 1; if (g_cx >= COLS) g_cx = COLS-1; }
    else if (cmd == 'D') { int n = atoi(g_esc_buf); g_cx -= n ? n : 1; if (g_cx < 0) g_cx = 0; }
    else if (cmd == 'P') {  /* DCH: delete characters */
        int n = atoi(g_esc_buf); if (!n) n = 1;
        int rem = COLS - g_cx - n;
        if (rem > 0) memmove(&g_cells[g_cy][g_cx], &g_cells[g_cy][g_cx + n], sizeof(Cell) * rem);
        for (int c = COLS - n; c < COLS; c++) g_cells[g_cy][c] = (Cell){ ' ', g_fg, g_bg };
    }
    g_esc = g_esc_bracket = false; g_esc_len = 0;
}

/* Process one byte from PTY output */
static void term_putc(uint8_t c) {
    if (g_esc) {
        if (!g_esc_bracket && c == '[') { g_esc_bracket = true; return; }
        if (g_esc_bracket) {
            if ((c >= 0x40 && c <= 0x7E) || g_esc_len >= (int)sizeof(g_esc_buf) - 1) {
                g_esc_buf[g_esc_len++] = c;
                g_esc_buf[g_esc_len]   = '\0';
                handle_esc_seq();
            } else {
                g_esc_buf[g_esc_len++] = c;
            }
        } else {
            /* Unhandled ESC + non-[ → skip */
            g_esc = false;
        }
        return;
    }
    if (c == 0x1B) { g_esc = true; g_esc_bracket = false; g_esc_len = 0; return; }
    if (c == '\r')  { g_cx = 0; return; }
    if (c == '\n')  { newline(); return; }
    if (c == '\t')  { g_cx = (g_cx + 8) & ~7; if (g_cx >= COLS) g_cx = COLS - 1; return; }
    if (c == 0x08 || c == 0x7F) {  /* BS/DEL */
        if (g_cx > 0) { g_cx--; g_cells[g_cy][g_cx] = (Cell){ ' ', g_fg, g_bg }; } return;
    }
    if (c < 0x20) return;  /* skip other controls */
    g_cells[g_cy][g_cx] = (Cell){ c, g_fg, g_bg };
    g_cx++;
    if (g_cx >= COLS) { g_cx = 0; g_cy++; if (g_cy >= ROWS) { g_cy = ROWS - 1; scroll_up(); } }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */
static uint32_t *g_fb = NULL;

static void fb_set(int x, int y, uint32_t col) {
    if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) g_fb[y * WIN_W + x] = col;
}

static void fb_fill(int x, int y, int w, int h, uint32_t col) {
    for (int row = y; row < y + h; row++)
        for (int col2 = x; col2 < x + w; col2++)
            fb_set(col2, row, col);
}

static void fb_glyph(int px, int py, uint8_t ch, uint32_t fg, uint32_t bg) {
    if (!g_glyph || ch >= (unsigned)g_n_glyph) return;
    const uint8_t *bits = g_glyph + ch * g_glyph_h;
    for (int row = 0; row < g_glyph_h; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < g_glyph_w; col++) {
            uint32_t c = (b & (0x80u >> col)) ? fg : bg;
            fb_set(px + col, py + row, c);
        }
    }
}

static void render(void) {
    /* Background */
    fb_fill(0, 0, WIN_W, WIN_H, g_bg);
    /* Title bar area (left for compositor) */
    fb_fill(0, 0, WIN_W, TITLE_H, 0xFF0e1620u);

    int cell_w = g_glyph_w + 1;
    int cell_h = g_glyph_h + 1;
    int ox = PAD, oy = TITLE_H + PAD;

    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            Cell *ce = &g_cells[row][col];
            int px = ox + col * cell_w;
            int py = oy + row * cell_h;
            if (px + cell_w > WIN_W) break;
            /* Cell background */
            fb_fill(px, py, cell_w, cell_h, ce->bg);
            fb_glyph(px, py, ce->ch, ce->fg, ce->bg);
        }
    }

    /* Cursor */
    if (g_cursor_vis) {
        int px = ox + g_cx * cell_w;
        int py = oy + g_cy * cell_h;
        fb_fill(px, py + cell_h - 2, cell_w, 2, g_fg);
    }
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */
static void ipc_send(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    write(fd, hdr, 8);
    if (len > 0 && data) write(fd, data, len);
}

static void send_frame(int fd) {
    uint32_t frm[4] = { 0, 0, WIN_W, WIN_H };
    uint32_t total  = 16 + WIN_W * WIN_H * 4;
    uint8_t *msg    = malloc(total);
    if (!msg) return;
    memcpy(msg,      frm,   16);
    memcpy(msg + 16, g_fb,  WIN_W * WIN_H * 4);
    ipc_send(fd, IPC_APP_FRAME, msg, total);
    free(msg);
}

/* ── PTY setup ───────────────────────────────────────────────────────────── */
static int pty_spawn(void) {
    g_pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (g_pty_master < 0) return -1;
    if (grantpt(g_pty_master) < 0 || unlockpt(g_pty_master) < 0) {
        close(g_pty_master); return -1;
    }
    char *slave_name = ptsname(g_pty_master);
    if (!slave_name) { close(g_pty_master); return -1; }

    /* Set terminal size */
    struct winsize ws = { .ws_row = ROWS, .ws_col = COLS, .ws_xpixel = 0, .ws_ypixel = 0 };
    ioctl(g_pty_master, TIOCSWINSZ, &ws);

    g_child_pid = fork();
    if (g_child_pid < 0) { close(g_pty_master); return -1; }
    if (g_child_pid == 0) {
        /* Child: set up PTY slave as controlling terminal */
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(1);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        /* Environment */
        setenv("TERM", "xterm", 1);
        setenv("PS1", "$ ", 1);
        char *argv[] = { "/bin/sh", NULL };
        execv("/bin/sh", argv);
        _exit(1);
    }
    /* Make pty non-blocking for reads */
    fcntl(g_pty_master, F_SETFL, O_NONBLOCK);
    return 0;
}

/* ── Key translation (FiFi → PTY byte sequences) ────────────────────────── */
static void key_to_pty(uint8_t k) {
    if (k >= 0x20 && k < 0x7F) { write(g_pty_master, &k, 1); return; }
    switch (k) {
    case 0x0D: case '\n': { uint8_t nl = '\r'; write(g_pty_master, &nl, 1); break; }
    case 0x08: case 0x7F: { uint8_t bs = 0x7F; write(g_pty_master, &bs, 1); break; }
    case 0x03: write(g_pty_master, "\x03", 1); break;  /* Ctrl+C */
    case 0x04: write(g_pty_master, "\x04", 1); break;  /* Ctrl+D */
    case 0x0C: write(g_pty_master, "\x0C", 1); break;  /* Ctrl+L */
    case 0x1A: write(g_pty_master, "\x1A", 1); break;  /* Ctrl+Z */
    case 0x1B: write(g_pty_master, "\x1B", 1); break;  /* ESC */
    case 0x09: write(g_pty_master, "\x09", 1); break;  /* Tab */
    /* Arrow keys (FIFI_KEY_* codes 0x80-0x83) */
    case 0x80: write(g_pty_master, "\x1B[D", 3); break;  /* Left */
    case 0x81: write(g_pty_master, "\x1B[C", 3); break;  /* Right */
    case 0x82: write(g_pty_master, "\x1B[A", 3); break;  /* Up */
    case 0x83: write(g_pty_master, "\x1B[B", 3); break;  /* Down */
    case 0x84: write(g_pty_master, "\x1B[3~", 4); break; /* Del */
    case 0x85: write(g_pty_master, "\x1B[H", 3); break;  /* Home */
    case 0x86: write(g_pty_master, "\x1B[F", 3); break;  /* End */
    default:
        if (k < 0x20) write(g_pty_master, &k, 1);  /* other ctrl chars */
        break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void) {
    font_load("/fifi-data/fonts/ter16b.psf");
    g_fb = malloc(WIN_W * WIN_H * 4);
    if (!g_fb) return 1;

    cell_clear_all();

    /* Spawn shell in PTY */
    if (pty_spawn() < 0) {
        /* fallback: show error message */
        const char *msg = "PTY spawn failed";
        for (int i = 0; msg[i]; i++) term_putc((uint8_t)msg[i]);
    }

    /* Connect to compositor */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;

    /* Register */
    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn,     &w, 2);
    memcpy(conn + 2, &h, 2);
    snprintf((char *)(conn + 4), 64, "Terminal");
    ipc_send(sock, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Wait for WIN_CREATED */
    {
        uint8_t hdr[8] = {0};
        if (read(sock, hdr, 8) < 8) return 1;
        uint32_t type, plen;
        memcpy(&type, hdr,     4);
        memcpy(&plen, hdr + 4, 4);
        if (type == IPC_WIN_CREATED && plen >= 20) {
            uint8_t resp[20]; read(sock, resp, plen > 20 ? 20 : plen);
        }
    }

    /* Initial render */
    render();
    send_frame(sock);

    fcntl(sock, F_SETFL, O_NONBLOCK);

    /* Event loop */
    uint8_t in_hdr[8];
    int     in_got = 0;
    uint8_t *in_pld = NULL;
    uint32_t in_plen = 0, in_pgot = 0;
    uint32_t in_type = 0;
    bool     running = true;
    uint32_t tick = 0;

    while (running) {
        /* Poll: compositor socket + PTY master */
        struct pollfd pfds[2];
        pfds[0].fd      = sock;
        pfds[0].events  = POLLIN;
        pfds[1].fd      = (g_pty_master >= 0) ? g_pty_master : -1;
        pfds[1].events  = POLLIN;

        int n = poll(pfds, 2, 16);  /* 16ms timeout ≈ 60Hz */
        if (n < 0 && errno == EINTR) continue;

        /* ── Read from compositor ── */
        if (pfds[0].revents & POLLIN) {
            uint8_t tbuf[256];
            ssize_t nr = read(sock, tbuf, sizeof(tbuf));
            if (nr <= 0) { running = false; break; }
            ssize_t pos = 0;
            while (pos < nr) {
                if (in_got < 8) {
                    in_hdr[in_got++] = tbuf[pos++];
                    if (in_got == 8) {
                        memcpy(&in_type,  in_hdr,     4);
                        memcpy(&in_plen,  in_hdr + 4, 4);
                        if (in_plen > 65536) { in_got = 0; break; }
                        if (in_plen > 0) { in_pld = malloc(in_plen); in_pgot = 0; }
                    }
                } else if (in_plen > 0 && in_pgot < in_plen) {
                    uint32_t need = in_plen - in_pgot;
                    uint32_t have = (uint32_t)(nr - pos);
                    uint32_t take = need < have ? need : have;
                    if (in_pld) memcpy(in_pld + in_pgot, tbuf + pos, take);
                    in_pgot += take; pos += take;
                    if (in_pgot >= in_plen) {
                        if (in_type == IPC_INPUT_KEY && in_plen >= 1) {
                            uint8_t key = in_pld ? in_pld[0] : 0;
                            if (g_pty_master >= 0) key_to_pty(key);
                        }
                        free(in_pld); in_pld = NULL;
                        in_got = 0; in_plen = 0; in_pgot = 0;
                    }
                } else {
                    if (in_type == IPC_INVALIDATE) g_dirty = true;
                    in_got = 0; in_plen = 0; in_pgot = 0;
                }
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) { running = false; break; }

        /* ── Read PTY output ── */
        if (g_pty_master >= 0 && (pfds[1].revents & POLLIN)) {
            uint8_t buf[512];
            ssize_t nr2;
            while ((nr2 = read(g_pty_master, buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < nr2; i++) term_putc(buf[i]);
                g_dirty = true;
            }
        }
        /* Check if child exited */
        if (g_child_pid > 0) {
            int wstat = 0;
            if (waitpid(g_child_pid, &wstat, WNOHANG) > 0) {
                g_child_pid = -1;
                const char *ex = "\r\n[Process exited]\r\n";
                for (const char *p = ex; *p; p++) term_putc((uint8_t)*p);
                g_dirty = true;
            }
        }

        /* ── Cursor blink ── */
        tick++;
        if ((tick % 30) == 0) { g_cursor_vis = !g_cursor_vis; g_dirty = true; }

        /* ── Render if dirty ── */
        if (g_dirty) {
            render();
            send_frame(sock);
            g_dirty = false;
        }
    }

    if (g_child_pid > 0) { kill(g_child_pid, SIGTERM); }
    if (g_pty_master >= 0) close(g_pty_master);
    ipc_send(sock, IPC_APP_CLOSE, NULL, 0);
    close(sock);
    free(g_fb);
    free(g_glyph);
    return 0;
}
