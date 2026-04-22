#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <termios.h>

/* FiFi kernel includes (via shadow headers + kernel/include) */
#include "console.h"
#include "gui.h"
#include "limine.h"

/* Linux platform functions */
void input_init(void);
void input_poll(void);
void input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h);
void mouse_init(void);
void mouse_cursor_update(void);
void vfs_init(void);
void pit_init(uint32_t hz);
void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm);

/* PTY functions */
void  pty_init(void);
void  pty_poll_output(void);
void  pty_write_input(uint8_t c);
int   pty_master_fd(void);
void  pty_set_winsize(uint16_t cols, uint16_t rows);

/* Input query functions */
bool  keyboard_gui_capture_active(void);
int   input_get_all_fds(int *buf, int maxn);
int   keyboard_try_getchar(void);

/* ── Framebuffer setup ───────────────────────────────────────────────────── */

static int      g_fb_fd   = -1;
static uint32_t *g_fb_mem = NULL;
static size_t   g_fb_size = 0;
static struct   limine_framebuffer g_lmfb;

static struct termios g_orig_term;
static bool           g_term_saved = false;

static void restore_term(void) {
    if (g_term_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

static void sig_handler(int sig) {
    (void)sig;
    restore_term();
    write(STDOUT_FILENO, "\033[?25h", 6);
    _exit(0);
}

static int fb_open(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) {
        fprintf(stderr, "[compositor] cannot open /dev/fb0\n");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "[compositor] FBIOGET_*SCREENINFO failed\n");
        return -1;
    }

    fprintf(stderr, "[compositor] fb0: %ux%u @ %ubpp pitch=%u\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "[compositor] need 32bpp (got %u)\n", vinfo.bits_per_pixel);
        return -1;
    }

    g_fb_size = finfo.smem_len;
    g_fb_mem  = (uint32_t *)mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        fprintf(stderr, "[compositor] mmap failed\n");
        return -1;
    }

    g_lmfb.address = g_fb_mem;
    g_lmfb.width   = vinfo.xres;
    g_lmfb.height  = vinfo.yres;
    g_lmfb.pitch   = finfo.line_length;
    g_lmfb.bpp     = (uint16_t)vinfo.bits_per_pixel;

    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    write(STDOUT_FILENO, "\033[?25l", 6);  /* hide cursor */

    if (tcgetattr(STDIN_FILENO, &g_orig_term) == 0) {
        g_term_saved = true;
        atexit(restore_term);
        struct termios raw = g_orig_term;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    if (fb_open() < 0) return 1;

    pit_init(100);
    pmm_init(NULL, 0);
    vfs_init();

    /* net_init detects NICs from /proc/net/dev */
    extern void net_init(void);
    net_init();

    console_init(&g_lmfb);
    console_backbuf_init();

    input_set_fb(g_fb_mem, (uint64_t)(g_lmfb.pitch / 4),
                 (int32_t)g_lmfb.width, (int32_t)g_lmfb.height);
    mouse_init();
    input_init();

    /* Start PTY shell before GUI so the shell prompt arrives early */
    pty_init();

    /* Set PTY size from actual framebuffer + expected terminal window geometry */
    {
        uint32_t fw = console_font_width();
        uint32_t fh = console_font_height();
        if (fw > 0 && fh > 0) {
            /* Terminal window is ~88% of fb width, ~90% of desk height (STATUS_H=20, TASKBAR_H=32) */
            uint64_t desk_h = g_lmfb.height > 52u ? g_lmfb.height - 52u : g_lmfb.height;
            uint64_t win_w  = g_lmfb.width * 88u / 100u;
            uint64_t win_h  = desk_h * 90u / 100u;
            /* Inner content after TITLE_H=24, BORDER=1, PAD=4 each side */
            uint64_t inner_w = win_w > 10u ? win_w - 10u : 1u;
            uint64_t inner_h = win_h > 33u ? win_h - 33u : 1u;
            uint16_t cols = (uint16_t)(inner_w / fw);
            uint16_t rows = (uint16_t)(inner_h / fh);
            if (cols < 20) cols = 20;
            if (rows < 5)  rows = 5;
            pty_set_winsize(cols, rows);
            fprintf(stderr, "[compositor] terminal %ux%u chars\n", cols, rows);
        }
    }

    gui_init();
    mouse_cursor_update();

    fprintf(stderr, "[compositor] running\n");

    /* Gather evdev fds for poll() */
    int evdev_fds[20];
    int nevdev = input_get_all_fds(evdev_fds, 20);

#define MAX_PFD 24
    struct pollfd pfd[MAX_PFD];

    for (;;) {
        /* ── Build poll set ────────────────────────────────────────────── */
        int nfds = 0;
        for (int i = 0; i < nevdev && nfds < MAX_PFD; i++) {
            pfd[nfds].fd     = evdev_fds[i];
            pfd[nfds].events = POLLIN;
            nfds++;
        }
        int pty_fd = pty_master_fd();
        if (pty_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = pty_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        /* Wait up to 16 ms (60 Hz) — wakes immediately on input */
        poll(pfd, (nfds_t)nfds, 16);

        /* ── PTY output → console buffer ───────────────────────────────── */
        pty_poll_output();

        /* ── evdev events ──────────────────────────────────────────────── */
        input_poll();

        /* ── Route keyboard to PTY when terminal window is focused ───────
         * gui_capture=false means the terminal is on top and visible;
         * keys sit in g_kb_ring waiting for the (now absent) bare-metal
         * shell.  We drain them here and write to the PTY instead.       */
        if (!keyboard_gui_capture_active()) {
            int c;
            while ((c = keyboard_try_getchar()) != -1)
                pty_write_input((uint8_t)c);
        }

        /* ── GUI tick ──────────────────────────────────────────────────── */
        gui_on_tick();

        /* ── Flip backbuffer → framebuffer, redraw cursor ─────────────── */
        if (console_flip_if_dirty())
            mouse_cursor_update();
    }

    return 0;
}
