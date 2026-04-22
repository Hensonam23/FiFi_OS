#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
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

/* Linux platform functions declared here */
void input_init(void);
void input_poll(void);
void input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h);
void mouse_init(void);
void mouse_cursor_update(void);
void vfs_init(void);
void pit_init(uint32_t hz);
void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm);

/* ── Framebuffer setup ───────────────────────────────────────────────────── */

static int      g_fb_fd   = -1;
static uint32_t *g_fb_mem = NULL;
static size_t   g_fb_size = 0;
static struct   limine_framebuffer g_lmfb;

static struct termios g_orig_term;
static bool           g_term_saved = false;

static void restore_term(void) {
    if (g_term_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    }
}

static void sig_handler(int sig) {
    (void)sig;
    restore_term();

    /* Show terminal cursor again */
    write(STDOUT_FILENO, "\033[?25h", 6);

    _exit(0);
}

static int fb_open(void) {
    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) {
        fprintf(stderr, "[compositor] cannot open /dev/fb0: check framebuffer driver\n");
        return -1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "[compositor] FBIOGET_*SCREENINFO failed\n");
        return -1;
    }

    fprintf(stderr, "[compositor] fb0: %ux%u @ %ubpp, pitch=%u\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "[compositor] need 32bpp framebuffer (got %u)\n",
                vinfo.bits_per_pixel);
        return -1;
    }

    g_fb_size = finfo.smem_len;
    g_fb_mem  = (uint32_t *)mmap(NULL, g_fb_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mem == MAP_FAILED) {
        fprintf(stderr, "[compositor] mmap /dev/fb0 failed\n");
        return -1;
    }

    /* Build limine_framebuffer struct */
    g_lmfb.address = g_fb_mem;
    g_lmfb.width   = vinfo.xres;
    g_lmfb.height  = vinfo.yres;
    g_lmfb.pitch   = finfo.line_length;
    g_lmfb.bpp     = (uint16_t)vinfo.bits_per_pixel;

    return 0;
}

/* ── 100 Hz sleep helper ─────────────────────────────────────────────────── */
#define TICK_NS 10000000L  /* 10 ms = 100 Hz */

static void sleep_tick(struct timespec *last) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ns = (long)((now.tv_sec  - last->tv_sec) * 1000000000L
                           + (now.tv_nsec - last->tv_nsec));
    long remain_ns  = TICK_NS - elapsed_ns;

    if (remain_ns > 0) {
        struct timespec ts = { 0, remain_ns };
        nanosleep(&ts, NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, last);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    /* Install signal handlers for clean exit */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Hide terminal cursor */
    write(STDOUT_FILENO, "\033[?25l", 6);

    /* Set terminal to raw mode (suppress input echo to console) */
    if (tcgetattr(STDIN_FILENO, &g_orig_term) == 0) {
        g_term_saved = true;
        atexit(restore_term);
        struct termios raw = g_orig_term;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /* Open framebuffer */
    if (fb_open() < 0) {
        fprintf(stderr, "[compositor] framebuffer init failed\n");
        return 1;
    }

    /* Init platform subsystems */
    pit_init(100);
    pmm_init(NULL, 0);
    vfs_init();

    /* Init console (double buffer) */
    console_init(&g_lmfb);
    console_backbuf_init();

    /* Init input */
    input_set_fb(g_fb_mem,
                 (uint64_t)(g_lmfb.pitch / 4),
                 (int32_t)g_lmfb.width,
                 (int32_t)g_lmfb.height);
    mouse_init();
    input_init();

    /* Init GUI */
    gui_init();

    /* Draw initial cursor */
    mouse_cursor_update();

    fprintf(stderr, "[compositor] running at 100 Hz\n");

    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    for (;;) {
        /* Poll evdev events */
        input_poll();

        /* Run one GUI tick */
        gui_on_tick();

        /* Flip backbuffer → framebuffer */
        if (console_flip_if_dirty()) {
            /* Redraw software cursor on top of flipped frame */
            mouse_cursor_update();
        }

        sleep_tick(&last);
    }

    return 0;
}
