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
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <termios.h>

/* FiFi kernel includes (via shadow headers + kernel/include) */
#include "console.h"
#include "gui.h"
#include "limine.h"
#include "mouse.h"

/* Linux platform functions */
void input_init(void);
void input_poll(void);
void input_rescan(void);
void input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h);
void mouse_init(void);
void mouse_cursor_update(void);
void vfs_init(void);
void pit_init(uint32_t hz);
void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm);

/* DRM/KMS backend (drm.c) — try first, fall back to /dev/fb0 */
struct limine_framebuffer *drm_open(void);
void drm_flush(void);
void drm_close(void);
void drm_blank_display(void);

/* IPC socket server for standalone FiFi apps (ipc.c) */
void ipc_init(void);
void ipc_poll(void);
void ipc_shutdown(void);
int  ipc_server_fd(void);

/* Wayland compositor (wayland.c) */
bool wayland_init(void);
void wayland_poll(void);
void wayland_shutdown(void);
int  wayland_server_fd(void);
void wayland_set_display_size(int w, int h);
void wayland_blit_surfaces(void);
void wayland_send_mouse(int32_t mx, int32_t my, uint8_t btns);
void wayland_send_key(uint32_t evdev_key, uint32_t state);
bool wayland_has_focus(void);
bool ipc_hit_test(int32_t mx, int32_t my);
bool ipc_drag_update(int32_t mx, int32_t my, bool lbtn);
bool ipc_try_close_at(int32_t mx, int32_t my);
void gui_draw_popups(void);
void gui_overdraw_top(void);
uint32_t gui_next_z(void);
uint32_t gui_topmost_z_at(int32_t mx, int32_t my);
uint32_t ipc_topmost_z(void);
uint32_t ipc_topmost_z_at(int32_t mx, int32_t my);
uint32_t ipc_topmost_z_in_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh);
bool gui_builtin_covers(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh, uint32_t ipc_z);
void gui_term_scroll_page(int dir);
void ipc_blit_all(void);
void ipc_draw_overlays(void);
void ipc_draw_resize_handles(void);
bool ipc_notify_draw(void);
bool ipc_keyboard_active(void);
void ipc_send_focused_key(uint8_t key);
void ipc_send_focused_mouse(int32_t mx, int32_t my, uint8_t btns);
void ipc_send_gamepad(uint16_t btns, int16_t lx, int16_t ly,
                      int16_t rx, int16_t ry, int16_t lt, int16_t rt);
void ipc_clear_focus(void);
void ipc_close_focused(void);
void ipc_cycle_focus(void);
void ipc_snap_focused(int zone);
bool ipc_resize_begin(int32_t mx, int32_t my);
bool ipc_resize_update(int32_t mx, int32_t my, bool lbtn);
bool ipc_resize_active(void);
bool ipc_resize_zone_at(int32_t mx, int32_t my);
bool ipc_file_drag_active(void);
void ipc_file_drag_update(int32_t mx, int32_t my);
void ipc_file_drag_drop(int32_t mx, int32_t my);
void ipc_file_drag_cancel(void);
void ipc_draw_drag_overlay(void);

/* Gamepad input query */
bool input_gamepad_connected(void);
bool input_gamepad_state(int idx, uint16_t *btns,
                         int16_t *lx, int16_t *ly,
                         int16_t *rx, int16_t *ry,
                         int16_t *lt, int16_t *rt);
bool input_gamepad_changed(int idx);

#define CUR_H 20   /* must match input.c / input_sdl.c */

/* PTY functions */
void  pty_init(void);
void  pty_poll_output(void);
void  pty_write_input(uint8_t c);
int   pty_master_fd(void);
void  pty_set_winsize(uint16_t cols, uint16_t rows);
void  pty_set_initial_winsize(uint16_t cols, uint16_t rows);

/* Input query functions */
bool  keyboard_gui_capture_active(void);
int   input_get_all_fds(int *buf, int maxn);
int   keyboard_try_getchar(void);
void  keyboard_clear_state(void);
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);

/* console internal: mark rows dirty so flip picks them up */
void  console_mark_dirty_rows(uint32_t y0, uint32_t y1);

/* ── Framebuffer setup ───────────────────────────────────────────────────── */

static int      g_fb_fd   = -1;
static uint32_t *g_fb_mem = NULL;
static size_t   g_fb_size = 0;
static struct   limine_framebuffer g_lmfb;
static bool     g_using_drm  = false;
static bool     g_gaming_mode = false;
static uint32_t g_fps_current = 0;
static bool     g_blanked     = false;
static bool     g_locked      = false;
static int      g_lock_timeout_s = 0;
static struct timespec g_last_input;
#define BLANK_TIMEOUT_S 300

/* ── Lock screen PIN state ───────────────────────────────────────────────── */
static char g_lock_buf[64];
static int  g_lock_buf_len  = 0;
static bool g_lock_bad      = false;  /* last PIN attempt was wrong */
static bool g_lock_pin_dirty = false; /* PIN input changed, overlay needs redraw */

int  compositor_lock_pin_len(void)   { return g_lock_buf_len; }
bool compositor_lock_bad_pin(void)   { bool v = g_lock_bad; return v; }
bool compositor_lock_pin_dirty(void) { bool v = g_lock_pin_dirty; g_lock_pin_dirty = false; return v; }

bool gaming_mode_active(void)        { return g_gaming_mode; }
uint32_t compositor_fps(void)        { return g_fps_current; }
bool compositor_locked(void)         { return g_locked; }
void compositor_lock(void)           { g_locked = true; g_blanked = false; g_lock_buf_len = 0; g_lock_bad = false; }
void compositor_unlock(void)         { g_locked = false; g_lock_buf_len = 0; g_lock_bad = false; }
void compositor_set_lock_timeout(int s) { g_lock_timeout_s = s; }
void gaming_mode_set(bool on)        {
    g_gaming_mode = on;
    fprintf(stderr, "[compositor] gaming mode %s\n", on ? "ON" : "OFF");
    const char *gov = on ? "performance" : "schedutil";
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1) ncpus = 8;
    for (int cpu = 0; cpu < ncpus; cpu++) {
        char path[80];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, gov, strlen(gov)); close(fd); }
    }
}

/* ── Threading: event thread (main) + render thread ────────────────────── */

static pthread_mutex_t g_mx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile bool   g_quit = false;

/* ── Terminal / signal setup ─────────────────────────────────────────────── */

static struct termios g_orig_term;
static bool           g_term_saved = false;

static void restore_term(void) {
    if (g_term_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

static void sig_handler(int sig) {
    (void)sig;
    g_quit = true;
    pthread_cond_signal(&g_cond);
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

/* ── Screenshot ──────────────────────────────────────────────────────────── */

static void take_screenshot(void) {
    static int s_idx = 0;
    mkdir("/fifi-data/screenshots", 0755);
    char path[64];
    snprintf(path, sizeof(path), "/fifi-data/screenshots/shot%03d.ppm", ++s_idx);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[screenshot] cannot open %s\n", path);
        return;
    }
    uint32_t w = g_lmfb.width, h = g_lmfb.height;
    uint32_t pitch32 = g_lmfb.pitch / 4;
    const uint32_t *fb = (const uint32_t *)g_lmfb.address;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = fb[y * pitch32 + x];
            uint8_t rgb[3] = { (px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[screenshot] saved %s\n", path);
    gui_toast_extern("Screenshot saved", 0x0080c8a0u);
}

/* ── Render thread ───────────────────────────────────────────────────────── */
/*
 * Runs compositing, flip, and DRM flush on a dedicated core so the event
 * thread (main) can continuously service I/O while rendering is in progress.
 *
 * Protocol: event thread signals g_cond each loop iteration under g_mx.
 * Render thread uses a timedwait capped at 16 ms (60 fps) so it also ticks
 * autonomously when there is no input.  DRM flush happens outside the mutex
 * so the event thread can do the next poll()+ipc_poll() concurrently.
 */
static void *render_thread_fn(void *arg)
{
    (void)arg;
    static int32_t s_last_cx = -1, s_last_cy = -1;
    struct timespec fps_ts;
    clock_gettime(CLOCK_MONOTONIC, &fps_ts);
    uint32_t fps_frames = 0;
    struct timespec last_render = {0};

    pthread_mutex_lock(&g_mx);
    while (!g_quit) {
        /*
         * Rate-limit to ~60 fps in normal mode, ~250 fps in gaming mode.
         * Deadline is anchored to last_render so early signals don't cause us
         * to render again before the next frame window is up.
         */
        long frame_ns = g_gaming_mode ? 4000000L : 16666666L;
        struct timespec deadline;
        long next_ns = last_render.tv_nsec + frame_ns;
        deadline.tv_sec  = last_render.tv_sec  + next_ns / 1000000000L;
        deadline.tv_nsec = next_ns % 1000000000L;

        /* Convert to REALTIME (timedwait uses CLOCK_REALTIME). */
        {
            struct timespec mono, real;
            clock_gettime(CLOCK_MONOTONIC, &mono);
            clock_gettime(CLOCK_REALTIME,  &real);
            long off_s  = real.tv_sec  - mono.tv_sec;
            long off_ns = real.tv_nsec - mono.tv_nsec;
            deadline.tv_sec  += off_s;
            deadline.tv_nsec += off_ns;
            if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
            if (deadline.tv_nsec < 0)            { deadline.tv_sec--; deadline.tv_nsec += 1000000000L; }
        }

        pthread_cond_timedwait(&g_cond, &g_mx, &deadline);
        if (g_quit) break;

        /* Skip render if we woke early (signal before frame window elapsed). */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec  - last_render.tv_sec)  * 1000L
                        + (now.tv_nsec - last_render.tv_nsec) / 1000000L;
        if (!g_gaming_mode && elapsed_ms < 14) continue;
        last_render = now;

        bool do_flush = false;

        if (!g_blanked) {
            struct timespec t0, t1, t2, t3;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            gui_on_tick();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ipc_blit_all();
            wayland_blit_surfaces();

            /* Erase old cursor position from backbuf before flip */
            int32_t cx, cy; bool lb, rb;
            mouse_get_state(&cx, &cy, &lb, &rb);
            bool cursor_moved = (cx != s_last_cx || cy != s_last_cy);
            if (cursor_moved && s_last_cy >= 0) {
                uint32_t ey0 = (uint32_t)(s_last_cy < 0 ? 0 : s_last_cy);
                console_mark_dirty_rows(ey0, ey0 + CUR_H);
            }

            ipc_draw_overlays();
            ipc_draw_resize_handles();
            ipc_draw_drag_overlay();
            ipc_notify_draw();
            gui_overdraw_top();           /* render built-in windows (Settings/Files/Viewer) above IPC */
            gui_draw_popups();

            clock_gettime(CLOCK_MONOTONIC, &t2);
            bool flipped = console_flip_if_dirty();
            clock_gettime(CLOCK_MONOTONIC, &t3);
            if (flipped || cursor_moved) {
                mouse_cursor_update();
                s_last_cx = cx; s_last_cy = cy;
            }

            /* Log slow frames and frame components for cursor lag diagnosis */
            {
                long tick_ms  = (t1.tv_sec - t0.tv_sec)*1000 + (t1.tv_nsec - t0.tv_nsec)/1000000;
                long flip_ms  = (t3.tv_sec - t2.tv_sec)*1000 + (t3.tv_nsec - t2.tv_nsec)/1000000;
                long total_ms = (t3.tv_sec - t0.tv_sec)*1000 + (t3.tv_nsec - t0.tv_nsec)/1000000;
                extern int g_redraw_src;
                if (total_ms >= 8)
                    fprintf(stderr, "[slow_frame] total=%ldms tick=%ldms flip=%ldms cx=%d cy=%d flipped=%d src=%d\n",
                            total_ms, tick_ms, flip_ms, cx, cy, (int)flipped, g_redraw_src);
            }

            do_flush = (flipped || cursor_moved) && g_using_drm;
        } else {
            /* Keep state ticking even when blanked (clock, animations). */
            gui_on_tick();
        }

        /* Refresh network IP display (rate-limited inside net_poll).
         * gui_on_tick() will pick up the new value and redraw the sysinfo widget. */
        {
            extern void net_poll(void);
            net_poll();
        }

        /* FPS counter: updated here since render thread owns the frame clock. */
        fps_frames++;
        {
            struct timespec fps_now;
            clock_gettime(CLOCK_MONOTONIC, &fps_now);
            long fps_elapsed = (fps_now.tv_sec  - fps_ts.tv_sec)  * 1000L
                             + (fps_now.tv_nsec - fps_ts.tv_nsec) / 1000000L;
            if (fps_elapsed >= 1000) {
                g_fps_current = (uint32_t)(fps_frames * 1000u / (uint32_t)fps_elapsed);
                fps_frames = 0;
                fps_ts = fps_now;
            }
        }

        /* Release the mutex while flushing so the event thread can do I/O. */
        if (do_flush) {
            pthread_mutex_unlock(&g_mx);
            drm_flush();
            pthread_mutex_lock(&g_mx);
        }
    }
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    write(STDOUT_FILENO, "\033[?25l", 6);

    if (tcgetattr(STDIN_FILENO, &g_orig_term) == 0) {
        g_term_saved = true;
        atexit(restore_term);
        struct termios raw = g_orig_term;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    struct limine_framebuffer *drm_fb = drm_open();
    if (drm_fb) {
        g_lmfb      = *drm_fb;
        g_using_drm = true;
        fprintf(stderr, "[compositor] DRM/KMS backend active\n");
    } else {
        fprintf(stderr, "[compositor] fallback: /dev/fb0\n");
        if (fb_open() < 0) return 1;
    }

    pit_init(100);
    pmm_init(NULL, 0);
    vfs_init();

    extern void net_init(void);
    net_init();

    extern bool hda_init(void);
    hda_init();

    console_init(&g_lmfb);
    console_backbuf_init();

    input_set_fb(g_lmfb.address, (uint64_t)(g_lmfb.pitch / 4),
                 (int32_t)g_lmfb.width, (int32_t)g_lmfb.height);
    mouse_init();
    input_init();

    gui_init();  /* loads resolution-appropriate font before terminal size is computed */
    mouse_cursor_update();

    /* Compute the terminal grid size, THEN spawn the shell at exactly that size.
     * Spawning first and resizing afterward sent a SIGWINCH that made the shell
     * reprint its prompt — the "double / # line" on first open. */
    {
        uint32_t fw = console_font_width();
        uint32_t fh = console_font_height();
        uint16_t cols = 80, rows = 24;
        if (fw > 0 && fh > 0) {
            uint64_t desk_h = g_lmfb.height > 52u ? g_lmfb.height - 52u : g_lmfb.height;
            uint64_t win_w  = g_lmfb.width * 88u / 100u;
            uint64_t win_h  = desk_h * 90u / 100u;
            uint64_t inner_w = win_w > 10u ? win_w - 10u : 1u;
            uint64_t inner_h = win_h > 33u ? win_h - 33u : 1u;
            cols = (uint16_t)(inner_w / fw);
            rows = (uint16_t)(inner_h / fh);
            if (cols < 20) cols = 20;
            if (rows < 5)  rows = 5;
        }
        pty_set_initial_winsize(cols, rows);
        pty_init();
        fprintf(stderr, "[compositor] terminal %ux%u chars\n", cols, rows);
    }

    ipc_init();

    wayland_set_display_size((int)g_lmfb.width, (int)g_lmfb.height);
    if (!wayland_init())
        fprintf(stderr, "[compositor] Wayland init failed — continuing without it\n");

    fprintf(stderr, "[compositor] running\n");

    int evdev_fds[20];
    int nevdev = input_get_all_fds(evdev_fds, 20);

#define MAX_PFD 24
    struct pollfd pfd[MAX_PFD];

    clock_gettime(CLOCK_MONOTONIC, &g_last_input);

    /* Spawn the render thread — it takes over all compositing + DRM flush. */
    pthread_t render_tid;
    pthread_create(&render_tid, NULL, render_thread_fn, NULL);

    /* ── Event loop: I/O only ────────────────────────────────────────────── */
    for (;;) {
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
        int ipc_fd = ipc_server_fd();
        if (ipc_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = ipc_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }
        int wl_fd = wayland_server_fd();
        if (wl_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = wl_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        /* poll() outside the mutex — render thread can flush concurrently. */
        poll(pfd, (nfds_t)nfds, g_gaming_mode ? 0 : 4);

        pthread_mutex_lock(&g_mx);

        /* ── IPC: accept new connections, read app frame messages ──────── */
        ipc_poll();
        wayland_poll();

        /* ── PTY output → console backbuf ──────────────────────────────── */
        pty_poll_output();

        /* ── Gamepad → focused IPC app ─────────────────────────────────── */
        if (ipc_keyboard_active() && input_gamepad_connected()) {
            for (int gi = 0; gi < 2; gi++) {
                if (!input_gamepad_changed(gi)) continue;
                uint16_t btns; int16_t lx, ly, rx, ry, lt, rt;
                if (input_gamepad_state(gi, &btns, &lx, &ly, &rx, &ry, &lt, &rt))
                    ipc_send_gamepad(btns, lx, ly, rx, ry, lt, rt);
            }
        }

        /* ── evdev: read input events into rings ───────────────────────── */
        /* Rescan for newly plugged devices every ~5 seconds */
        static int _rescan_ticks = 0;
        if (++_rescan_ticks >= 300) { _rescan_ticks = 0; input_rescan(); }
        int32_t px, py; bool pb_l, pb_r;
        mouse_get_state(&px, &py, &pb_l, &pb_r);
        input_poll();
        int32_t nx, ny; bool nb_l, nb_r;
        mouse_get_state(&nx, &ny, &nb_l, &nb_r);
        bool had_input = (nx != px || ny != py || nb_l != pb_l || nb_r != pb_r);

        /* ── Mouse routing ──────────────────────────────────────────────── */
        {
            int32_t mcx, mcy; bool mlb, mrb;
            mouse_get_state(&mcx, &mcy, &mlb, &mrb);
            uint8_t btns = (mlb ? 1 : 0) | (mrb ? 2 : 0);

            if (ipc_file_drag_active()) {
                ipc_file_drag_update(mcx, mcy);
                if (!mlb) ipc_file_drag_drop(mcx, mcy);
                goto mouse_done;
            }

            bool resizing = ipc_resize_update(mcx, mcy, mlb);
            bool dragging = !resizing && ipc_drag_update(mcx, mcy, mlb);

            if (mlb && !pb_l && !dragging && !resizing) {
                /* Single cross-system topmost decision: whichever layer has the higher
                 * raise_z at the cursor owns the click. The terminal is INCLUDED here so
                 * input matches what is visually on top (gui_overdraw_top now paints the
                 * terminal over IPC when its raise_z is highest). When the terminal is on
                 * top it owns the click; when an IPC app is on top (it covers the terminal)
                 * the IPC app owns it. Use the taskbar to bring a terminal-covered app back. */
                uint32_t gui_z = gui_topmost_z_at(mcx, mcy);
                uint32_t ipc_z = ipc_topmost_z_at(mcx, mcy);
                if (ipc_z > gui_z) {
                    /* IPC window is on top here → route to the IPC layer. */
                    if (!ipc_try_close_at(mcx, mcy))
                        if (!ipc_resize_begin(mcx, mcy))
                            ipc_hit_test(mcx, mcy);
                } else {
                    /* A built-in window / terminal / empty desktop is on top here.
                     * gui_on_tick() does the actual built-in raise/drag/button work this
                     * same frame; we only drop IPC keyboard focus. */
                    ipc_clear_focus();
                }
            }
            if (ipc_keyboard_active() && !dragging && !resizing)
                ipc_send_focused_mouse(mcx, mcy, btns);
            mouse_done:;

            if (!ipc_keyboard_active())
                wayland_send_mouse(mcx, mcy, btns);
        }

        /* ── Activity tracking ──────────────────────────────────────────── */
        if (had_input)
            clock_gettime(CLOCK_MONOTONIC, &g_last_input);
        if (g_blanked && had_input)
            g_blanked = false;

        /* ── Keyboard routing ───────────────────────────────────────────── */
        {
            __attribute__((weak)) void gui_show_desktop(void);
            /* When locked: if /fifi-data/lock-pin exists, collect a PIN and
             * validate on Enter. If no PIN file, any key unlocks (no-auth mode). */
            if (g_locked) {
                bool has_pin = (access("/fifi-data/lock-pin", F_OK) == 0);
                int lc;
                while ((lc = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    uint8_t uc = (uint8_t)lc;
                    if (!has_pin) {
                        /* No PIN configured — any key unlocks */
                        compositor_unlock();
                        break;
                    } else if (uc == '\r' || uc == '\n') {
                        g_lock_buf[g_lock_buf_len] = '\0';
                        FILE *pf = fopen("/fifi-data/lock-pin", "r");
                        if (pf) {
                            char pin[64] = {0};
                            bool ok = false;
                            if (fgets(pin, sizeof(pin), pf)) {
                                int pl = (int)strlen(pin);
                                while (pl > 0 && (pin[pl-1] == '\n' || pin[pl-1] == '\r'))
                                    pin[--pl] = '\0';
                                ok = (strcmp(g_lock_buf, pin) == 0);
                            }
                            fclose(pf);
                            if (ok) { compositor_unlock(); break; }
                            else    { g_lock_bad = true; g_lock_buf_len = 0; }
                        }
                    } else if (uc == 0x08u || uc == 0x7fu) {
                        if (g_lock_buf_len > 0) g_lock_buf_len--;
                        g_lock_bad = false;
                    } else if (uc >= 0x20u && uc < 0x7fu && g_lock_buf_len < 63) {
                        g_lock_buf[g_lock_buf_len++] = (char)uc;
                        g_lock_bad = false;
                    }
                    g_lock_pin_dirty = true;
                }
                keyboard_clear_state();
                goto keyboard_done;
            }
            if (ipc_keyboard_active()) {
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    uint8_t uc = (uint8_t)c;
                    if (uc == 0x96u) { take_screenshot(); continue; }
                    if (uc == 0x97u) { ipc_close_focused(); continue; }
                    if (uc == 0x89u) { ipc_cycle_focus(); continue; }
                    /* 0x17 (Ctrl+W) passes through to IPC app — terminals handle tab/window close */
                    if (uc == 0x98u) { ipc_snap_focused(1); continue; }
                    if (uc == 0x99u) { ipc_snap_focused(2); continue; }
                    if (uc == 0x9Au) { ipc_snap_focused(3); continue; }
                    if (uc == 0x9Bu) { ipc_snap_focused(0); continue; }
                    if (uc == 0x9Cu) { compositor_lock(); continue; }
                    if (uc == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                    if (uc == 0x1Bu && ipc_file_drag_active()) { ipc_file_drag_cancel(); continue; }
                    if (uc >= 0x8Au && uc <= 0x90u) {
                        /* F1-F7: already in GUI ring via kb_push_internal — just consume */
                        continue;
                    } else {
                        ipc_send_focused_key(uc);
                    }
                }
            } else if (!keyboard_gui_capture_active()) {
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                    if ((uint8_t)c == 0x97u) { ipc_close_focused(); continue; }
                    if ((uint8_t)c == 0x89u) { ipc_cycle_focus(); continue; }
                    if ((uint8_t)c == 0x98u) { ipc_snap_focused(1); continue; }
                    if ((uint8_t)c == 0x99u) { ipc_snap_focused(2); continue; }
                    if ((uint8_t)c == 0x9Au) { ipc_snap_focused(3); continue; }
                    if ((uint8_t)c == 0x9Bu) { ipc_snap_focused(0); continue; }
                    if ((uint8_t)c == 0x9Cu) { compositor_lock(); continue; }
                    if ((uint8_t)c == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                    if ((uint8_t)c == 0x87u) { gui_term_scroll_page(+1); continue; }
                    if ((uint8_t)c == 0x88u) { gui_term_scroll_page(-1); continue; }
                    pty_write_input((uint8_t)c);
                }
            } else {
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                    if ((uint8_t)c == 0x97u) { ipc_close_focused(); continue; }
                    if ((uint8_t)c == 0x89u) { ipc_cycle_focus(); continue; }
                    if ((uint8_t)c == 0x98u) { ipc_snap_focused(1); continue; }
                    if ((uint8_t)c == 0x99u) { ipc_snap_focused(2); continue; }
                    if ((uint8_t)c == 0x9Au) { ipc_snap_focused(3); continue; }
                    if ((uint8_t)c == 0x9Bu) { ipc_snap_focused(0); continue; }
                    if ((uint8_t)c == 0x9Cu) { compositor_lock(); continue; }
                    if ((uint8_t)c == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                }
            }
        }
        keyboard_done:;

        /* ── Screen blank / auto-lock check ────────────────────────────── */
        {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long idle_s = now_ts.tv_sec - g_last_input.tv_sec;
            if (!g_locked && g_lock_timeout_s > 0 && idle_s >= g_lock_timeout_s) {
                compositor_lock();
                fprintf(stderr, "[compositor] auto-locked (idle %lds)\n", idle_s);
            }
            if (!g_blanked && !g_gaming_mode && idle_s >= BLANK_TIMEOUT_S) {
                g_blanked = true;
                if (g_using_drm) drm_blank_display();
                else if (g_fb_mem) memset(g_fb_mem, 0, g_fb_size);
                fprintf(stderr, "[compositor] display blanked (idle %lds)\n", idle_s);
            }
        }

        /* Signal the render thread — new I/O may have updated frame data. */
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_mx);
    }

    g_quit = true;
    pthread_cond_signal(&g_cond);
    pthread_join(render_tid, NULL);
    return 0;
}
