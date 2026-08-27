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
#include "../platform/linux/vendor/lodepng.h"
#include "limine.h"
#include "mouse.h"
#include "xwm.h"     /* rootless-XWayland window manager */
#include "event_handoff.h"

/* Linux platform functions */
void input_init(void);
void input_poll(void);
void input_poll_motion(void);
void input_poll_controls(void);
void input_flush_deferred_clicks(void);
void input_rescan(void);
int input_hotplug_fd(void);
bool input_hotplug_pending(void);
void input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h);
void mouse_init(void);
void mouse_cursor_update(void);
void vfs_init(void);
void pit_init(uint32_t hz);
void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm);

/* DRM/KMS backend (drm.c) — try first, fall back to /dev/fb0 */
struct limine_framebuffer *drm_open(void);
void drm_flush(void);
bool drm_cursor_enabled(void);
void drm_cursor_move(int32_t x, int32_t y);
void drm_cursor_set_visible(bool visible);
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
void wayland_send_mouse(int32_t mx, int32_t my, uint8_t btns,
                        double dx, double dy,
                        double dx_unaccel, double dy_unaccel);
bool wayland_pointer_locked(void);
void wayland_release_pointer_lock(void);
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
bool wayland_snap_focused(int zone);
bool wayland_close_focused(void);
void gui_close_focused_win(void);

/* Alt+F4: close whatever the user considers the active window, trying the
 * layers in the same priority order as key routing and snapping. */
static void close_focused_window(void) {
    if (ipc_keyboard_active()) { ipc_close_focused(); return; }
    if (wayland_close_focused()) return;
    gui_close_focused_win();
}
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
int   input_get_pointer_fds(int *buf, int maxn);
int   input_get_control_fds(int *buf, int maxn);
int   keyboard_try_getchar(void);
void  keyboard_clear_state(void);
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);
void  input_consume_relative_motion(double *dx, double *dy,
                                    double *dx_unaccel, double *dy_unaccel);
bool  input_consume_pointer_unlock_request(void);

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
#define BLANK_TIMEOUT_S 3600  /* 1 hour — extended for browser testing */

/* ── Lock screen PIN state ───────────────────────────────────────────────── */
static char g_lock_buf[64];
static int  g_lock_buf_len  = 0;
static bool g_lock_bad      = false;  /* last PIN attempt was wrong */
static bool g_lock_pin_dirty = false; /* PIN input changed, overlay needs redraw */

int  compositor_lock_pin_len(void)   { return g_lock_buf_len; }
bool compositor_lock_bad_pin(void)   { return g_lock_bad; }
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

/* Put the machine on the "performance" governor and unlock turbo at startup so
 * the desktop never idles at the CPU's minimum frequency (the stock powersave
 * governor pinned it at ~800MHz). This mirrors the boot-init perf block for the
 * common case where only the compositor binary is redeployed (no image rebuild).
 * All best-effort: a missing knob is simply skipped ("no matter the system"). */
static void apply_perf_defaults(void) {
    int ncpus = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1) ncpus = 8;
    for (int cpu = 0; cpu < ncpus; cpu++) {
        char path[96]; int fd;
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, "performance", 11); close(fd); }
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", cpu);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, "performance", 11); close(fd); }
    }
    int fd = open("/sys/devices/system/cpu/intel_pstate/no_turbo", O_WRONLY);
    if (fd >= 0) { write(fd, "0", 1); close(fd); }
    fd = open("/sys/devices/system/cpu/cpufreq/boost", O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }
}

/* ── Threading: event thread (main) + render thread ────────────────────── */

static pthread_mutex_t g_mx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
/* Set before the event thread waits for g_mx.  A render that overruns its
 * frame deadline must explicitly hand the mutex over instead of immediately
 * beginning another frame and making all pointer devices appear to stall. */
static fifi_event_handoff_t g_event_handoff = FIFI_EVENT_HANDOFF_INIT;
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
    /* Async-signal context: do NOT call pthread_cond_signal — if the signal
     * interrupts a thread inside the condvar internals, re-entering them can
     * self-deadlock or corrupt state. We are terminating via _exit anyway.
     * restore_term()+write() are the minimal cleanup to leave the TTY usable. */
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
    snprintf(path, sizeof(path), "/fifi-data/screenshots/shot%03d.png", ++s_idx);
    uint32_t w = g_lmfb.width, h = g_lmfb.height;
    uint32_t pitch32 = g_lmfb.pitch / 4;
    const uint32_t *fb = (const uint32_t *)g_lmfb.address;
    unsigned char *rgba = malloc((size_t)w * h * 4u);
    if (!rgba) return;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = fb[y * pitch32 + x];
            unsigned char *d = rgba + ((size_t)y * w + x) * 4u;
            d[0] = (px >> 16) & 0xFF; d[1] = (px >> 8) & 0xFF;
            d[2] = px & 0xFF;         d[3] = 0xFF;
        }
    unsigned char *png = NULL; size_t png_size = 0;
    unsigned err = lodepng_encode32(&png, &png_size, rgba, w, h);
    free(rgba);
    if (err || !png) {
        fprintf(stderr, "[screenshot] png encode failed (%u)\n", err);
        free(png);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(png, 1, png_size, f);
        fclose(f);
        fprintf(stderr, "[screenshot] saved %s (%zu bytes)\n", path, png_size);
        gui_toast_extern("Screenshot saved", 0x0080c8a0u);
    } else {
        fprintf(stderr, "[screenshot] cannot open %s\n", path);
    }
    free(png);
}

/* DEV: kill -USR1 <pid> requests a screenshot (handled in the event loop). */
static volatile sig_atomic_t g_want_shot = 0;
static void shot_sig_handler(int sig) { (void)sig; g_want_shot = 1; }

/* ── Pointer thread ──────────────────────────────────────────────────────── */
/*
 * Physical pointer devices have a dedicated event-driven reader.  It never
 * takes the compositor mutex, so app frames, PTY output, Wayland/X11 traffic,
 * filesystem work, and software rendering cannot delay evdev consumption.
 * Button edges are deferred by input.c until the desktop event thread can
 * route them against a consistent window stack.
 */
static void *pointer_thread_fn(void *arg)
{
    (void)arg;
    int pointer_fds[64];
    struct pollfd pointer_poll[64];

    while (!g_quit) {
        int count = input_get_pointer_fds(pointer_fds, 64);
        if (count == 0) {
            const struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000000L };
            nanosleep(&pause, NULL);
            continue;
        }
        for (int i = 0; i < count; i++) {
            pointer_poll[i].fd = pointer_fds[i];
            pointer_poll[i].events = POLLIN;
            pointer_poll[i].revents = 0;
        }
        int ready = poll(pointer_poll, (nfds_t)count, 50);
        if (ready <= 0) continue;

        int32_t old_x, old_y; bool old_l, old_r;
        mouse_get_state(&old_x, &old_y, &old_l, &old_r);
        input_poll_motion();
        int32_t new_x, new_y; bool new_l, new_r;
        mouse_get_state(&new_x, &new_y, &new_l, &new_r);
        if (new_x == old_x && new_y == old_y) continue;

        if (drm_cursor_enabled()) drm_cursor_move(new_x, new_y);
        if (!drm_cursor_enabled()) mouse_cursor_update();
    }
    return NULL;
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
            gui_on_tick();
            /* When a Wayland window (browser) is showing, repaint the desktop
             * beneath it every frame so its transparent CSD shadow margin blends
             * over the wallpaper instead of stale black. */
            { extern bool wayland_any_mapped(void); extern void full_redraw(void);
              if (wayland_any_mapped()) full_redraw(); }
            ipc_blit_all();
            wayland_blit_surfaces();
            /* Lift a focused IPC window (e.g. the App Store) above Wayland/XWayland
             * windows when it was raised more recently, matching the cross-layer
             * input routing. No-op unless an IPC window's z_order beats the Wayland
             * layer, so it costs nothing in the common case. */
            { extern bool wayland_any_mapped(void); extern void ipc_overdraw_top(void);
              if (wayland_any_mapped()) ipc_overdraw_top(); }
            /* Panels stay above app windows (like any desktop shell): repaint the
             * status bar + taskbar over whatever the Wayland layer just blitted. */
            { extern bool wayland_any_mapped(void);
              extern void draw_status_bar(void); extern void taskbar_draw(void);
              if (wayland_any_mapped()) { draw_status_bar(); taskbar_draw(); } }

            /* Erase old cursor position from backbuf before flip */
            int32_t cx, cy; bool lb, rb;
            mouse_get_state(&cx, &cy, &lb, &rb);
            bool cursor_moved = (cx != s_last_cx || cy != s_last_cy);
            bool hw_cursor = drm_cursor_enabled();
            bool pointer_locked = wayland_pointer_locked();
            static bool s_pointer_locked = false;
            bool lock_changed = pointer_locked != s_pointer_locked;
            if (!hw_cursor &&
                ((!pointer_locked && cursor_moved) ||
                 (pointer_locked && lock_changed)) && s_last_cy >= 0) {
                uint32_t ey0 = (uint32_t)s_last_cy;
                console_mark_dirty_rows(ey0, ey0 + CUR_H);
            }

            ipc_draw_overlays();
            ipc_draw_resize_handles();
            ipc_draw_drag_overlay();
            ipc_notify_draw();
            gui_overdraw_top();           /* render built-in windows (Settings/Files/Viewer) above IPC */
            gui_draw_popups();

            /* When Wayland windows are composited we full_redraw() + blit the
             * whole desktop each frame; the per-rect dirty tracking can miss
             * rows a moved/minimized window vacated, leaving stale trails in
             * VRAM. Force the whole screen dirty so the flip always mirrors the
             * backbuffer exactly. (Also covers the frame right after the last
             * Wayland window closes, clearing its final footprint.) */
            {
                extern bool wayland_any_mapped(void);
                static bool s_wl_prev = false;
                bool wl_now = wayland_any_mapped();
                if (wl_now || s_wl_prev)
                    console_mark_dirty_rows(0, (uint32_t)g_lmfb.height);
                s_wl_prev = wl_now;
            }
            bool flipped = console_flip_if_dirty();
            if (!hw_cursor && !pointer_locked &&
                (flipped || cursor_moved || lock_changed)) {
                mouse_cursor_update();
            }
            s_last_cx = cx; s_last_cy = cy;
            s_pointer_locked = pointer_locked;

            do_flush = (flipped || (!hw_cursor && !pointer_locked &&
                                    (cursor_moved || lock_changed))) &&
                       g_using_drm;
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

        /* A slow software-composited frame can exceed the next deadline.  In
         * that case pthread_cond_timedwait() returns immediately and this
         * thread used to retain g_mx across consecutive frames, starving the
         * event thread and briefly freezing every mouse/touchpad.  Guarantee
         * one event-loop handoff after a frame whenever input is waiting. */
        fifi_event_handoff_yield(&g_event_handoff, &g_mx);
    }
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, shot_sig_handler);

    apply_perf_defaults();   /* full CPU frequency from the first frame */

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
    if (drm_cursor_enabled()) drm_cursor_move(g_lmfb.width / 2, g_lmfb.height / 2);
    else mouse_cursor_update();

    /* Compute the terminal grid size, THEN spawn the shell at exactly that size.
     * Spawning first and resizing afterward sent a SIGWINCH that made the shell
     * reprint its prompt — the "double / # line" on first open.
     * Use the actual console dimensions (set by gui_init/console_load_psf) so the
     * PTY matches what is rendered — a geometry-derived guess gave fewer rows than
     * the console actually rendered, causing Ink TUI ghost text. */
    {
        /* The terminal boots hidden, so the console viewport (and console_cols())
         * is 0 here — derive the initial grid from the full framebuffer instead,
         * giving the shell a sensible full-width size. term_set_viewport then
         * re-syncs the PTY to the actual window size when the terminal opens. */
        uint16_t fwq = (uint16_t)console_font_width();
        uint16_t fhq = (uint16_t)console_font_height();
        uint16_t cols = fwq ? (uint16_t)(console_fb_width()  / fwq) : 80;
        uint16_t rows = fhq ? (uint16_t)(console_fb_height() / fhq) : 25;
        /* Reserve 2 rows for the taskbar at the bottom */
        if (rows > 2) rows -= 2;
        if (cols < 20) cols = 20;
        if (rows < 5)  rows = 5;
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
    int nevdev = input_get_control_fds(evdev_fds, 20);

#define MAX_PFD 32
    struct pollfd pfd[MAX_PFD];

    clock_gettime(CLOCK_MONOTONIC, &g_last_input);

    /* Pointer I/O and rendering run independently from desktop event routing. */
    pthread_t pointer_tid, render_tid;
    pthread_create(&pointer_tid, NULL, pointer_thread_fn, NULL);
    pthread_create(&render_tid, NULL, render_thread_fn, NULL);

    int32_t routed_x, routed_y; bool routed_l, routed_r;
    mouse_get_state(&routed_x, &routed_y, &routed_l, &routed_r);

    int s_xwm_probe = 0;   /* throttles X-WM connect attempts while X is down */

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
        /* Rootless-XWayland window manager: poll its X socket alongside Wayland
         * so LibreOffice and other X11 apps get their own decorated windows. */
        int x_fd = xwm_fd();
        if (x_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = x_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }
        int hotplug_fd = input_hotplug_fd();
        if (hotplug_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = hotplug_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        /* poll() outside the mutex — render thread can flush concurrently.
         * Never use timeout 0: that made gaming mode busy-poll a whole core for
         * no benefit (the render thread paces frames itself; input wakes poll
         * immediately regardless). 2ms = 500Hz input sampling in gaming. */
        poll(pfd, (nfds_t)nfds, g_gaming_mode ? 2 : 4);

        bool pb_l = routed_l;
        bool had_input = false;

        fifi_event_handoff_request(&g_event_handoff);
        while (pthread_mutex_trylock(&g_mx) != 0) {
            const struct timespec input_pause = {
                .tv_sec = 0,
                .tv_nsec = g_gaming_mode ? 2000000 : 4000000,
            };
            nanosleep(&input_pause, NULL);
        }
        fifi_event_handoff_acquired(&g_event_handoff);
        input_flush_deferred_clicks();
        input_poll_controls();
        if (input_consume_pointer_unlock_request())
            wayland_release_pointer_lock();

        /* App frame traffic and PTY output can be comparatively expensive;
         * physical pointer motion has already been consumed above. */
        if (input_hotplug_pending()) {
            input_rescan();
            /* Rebuild the poll set. input_rescan() closes the fds of unplugged
             * devices; leaving those stale fds in pfd made poll() return
             * immediately with POLLNVAL every iteration, spinning the event
             * thread at 100% CPU. This also picks up newly plugged devices. */
            nevdev = input_get_control_fds(evdev_fds, 20);
        }
        /* ── IPC: accept new connections, read app frame messages ──────── */
        ipc_poll();
        wayland_poll();
        /* Lazily connect the X window manager once XWayland is up (started on
         * demand by fifi-run); retried at ~40ms cadence while not connected. */
        if (xwm_active()) {
            xwm_poll();
        } else if (++s_xwm_probe >= 10) {   /* ~40ms: attach before the app maps */
            s_xwm_probe = 0;
            xwm_init();
        }

        if (g_want_shot) { g_want_shot = 0; take_screenshot(); }

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

        /* ── Mouse routing ──────────────────────────────────────────────── */
        {
            int32_t mcx, mcy; bool mlb, mrb;
            double rel_dx, rel_dy, rel_dx_unaccel, rel_dy_unaccel;
            mouse_get_state(&mcx, &mcy, &mlb, &mrb);
            input_consume_relative_motion(&rel_dx, &rel_dy,
                                          &rel_dx_unaccel, &rel_dy_unaccel);
            had_input = mcx != routed_x || mcy != routed_y ||
                        mlb != routed_l || mrb != routed_r ||
                        rel_dx != 0.0 || rel_dy != 0.0;
            uint8_t btns = (mlb ? 1 : 0) | (mrb ? 2 : 0);

            /* Cross-layer z-order: the Wayland browser participates in the same
             * raise_z ordering as built-in and IPC windows, so clicking chooses the
             * genuinely-topmost layer at the cursor.
             * gui_z/ipc_z/wl_top are static because the file-drag goto below jumps
             * over their assignments while the tail after mouse_done still reads
             * them — statics keep the last non-drag frame's values (reading autos
             * there was undefined behaviour). */
            extern bool wayland_covers(int32_t, int32_t);
            extern uint32_t gui_wl_z(void);
            extern void gui_wl_raise(void);
            extern bool wayland_any_mapped(void);
            static uint32_t gui_z, ipc_z;
            static bool wl_top;

            if (ipc_file_drag_active()) {
                ipc_file_drag_update(mcx, mcy);
                if (!mlb) ipc_file_drag_drop(mcx, mcy);
                goto mouse_done;
            }

            bool resizing = ipc_resize_update(mcx, mcy, mlb);
            bool dragging = !resizing && ipc_drag_update(mcx, mcy, mlb);

            gui_z = gui_topmost_z_at(mcx, mcy);
            ipc_z = ipc_topmost_z_at(mcx, mcy);
            bool wl_cover  = wayland_any_mapped() && wayland_covers(mcx, mcy);
            uint32_t wl_z  = wl_cover ? gui_wl_z() : 0;
            wl_top = wl_cover && wl_z >= gui_z && wl_z >= ipc_z;

            if (mlb && !pb_l && !dragging && !resizing) {
                if (wl_top) {
                    /* Click on the browser → raise it above the built-in/IPC windows
                     * and drop IPC focus. gui_on_tick() skips its own raise when the
                     * browser is on top here (see gui_topmost check there). */
                    gui_wl_raise();
                    ipc_clear_focus();
                } else if (ipc_z > gui_z) {
                    /* IPC window is on top here → route to the IPC layer. Drop
                     * Wayland keyboard focus so keys reach the IPC app (App Store
                     * search) instead of a Wayland app still open behind it. */
                    extern void wayland_clear_kbd_focus(void);
                    wayland_clear_kbd_focus();
                    if (!ipc_try_close_at(mcx, mcy))
                        if (!ipc_resize_begin(mcx, mcy))
                            ipc_hit_test(mcx, mcy);
                } else {
                    /* A built-in window / terminal / empty desktop is on top here.
                     * gui_on_tick() does the actual built-in raise/drag/button work. */
                    extern void wayland_clear_kbd_focus(void);
                    wayland_clear_kbd_focus();
                    ipc_clear_focus();
                }
            }

            /* ipc_resize_active() re-check: a resize may have just STARTED on this
             * press (ipc_resize_begin above) after `resizing` was computed — the
             * grab click must not leak into the app as a content click. Same for
             * titlebar presses (drag start / double-click maximize): consumed. */
            {
                extern bool ipc_press_suppressed(bool lbtn_down);
                /* Only forward when the pointer actually moved or a button changed —
                 * previously this fired every loop iteration, flooding the focused app
                 * with ~125 identical mouse events/sec (wasted work + a framing-desync
                 * risk under load). */
                if (had_input && ipc_keyboard_active() && !dragging && !resizing &&
                    !ipc_resize_active() && !ipc_press_suppressed(mlb))
                    ipc_send_focused_mouse(mcx, mcy, btns);
            }
            mouse_done:;

            /* Feed the browser input only when it is the topmost layer under the
             * cursor (so clicks on a raised built-in window over it don't leak). */
            if (wayland_any_mapped() &&
                (wayland_pointer_locked() || wl_top ||
                 (gui_z == 0 && ipc_z == 0)))
                wayland_send_mouse(mcx, mcy, btns, rel_dx, rel_dy,
                                   rel_dx_unaccel, rel_dy_unaccel);
            /* Scroll wheel → focused Wayland surface (browser). Consume it here so the
             * desktop/terminal don't also scroll from the same wheel motion. */
            if (wayland_has_focus()) {
                extern int8_t mouse_consume_scroll(void);
                extern void wayland_send_scroll(int8_t dir);
                int8_t sc = mouse_consume_scroll();
                if (sc) wayland_send_scroll(sc);
            }
            routed_x = mcx; routed_y = mcy;
            routed_l = mlb; routed_r = mrb;
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
            if (ipc_keyboard_active() && !wayland_has_focus()) {
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    uint8_t uc = (uint8_t)c;
                    if (uc == 0x96u) { take_screenshot(); continue; }
                    if (uc == 0x97u) { close_focused_window(); continue; }
                    if (uc == 0x89u) { ipc_cycle_focus(); continue; }
                    /* 0x17 (Ctrl+W) passes through to IPC app — terminals handle tab/window close */
                    if (uc == 0x98u) { ipc_snap_focused(1); continue; }
                    if (uc == 0x99u) { ipc_snap_focused(2); continue; }
                    if (uc == 0x9Au) { ipc_snap_focused(3); continue; }
                    if (uc == 0x9Bu) { ipc_snap_focused(0); continue; }
                    if (uc == 0x9Cu) { compositor_lock(); continue; }
                    if (uc == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                    if (uc == 0x9Eu || uc == 0x9Fu) { continue; }  /* Super tap/help — handled in the GUI ring */
                    if (uc == 0x1Bu && ipc_file_drag_active()) { ipc_file_drag_cancel(); continue; }
                    if (uc >= 0x8Au && uc <= 0x90u) {
                        /* F1-F7: already in GUI ring via kb_push_internal — just consume */
                        continue;
                    } else {
                        ipc_send_focused_key(uc);
                    }
                }
            } else if (!keyboard_gui_capture_active() && !wayland_has_focus()) {
                /* PTY terminal path — skipped when Wayland (browser) has focus to
                 * prevent F-keys from spawning system apps (fifi-browser etc.) while
                 * the browser is running. Raw keys already forwarded to Wayland above. */
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                    if ((uint8_t)c == 0x97u) { close_focused_window(); continue; }
                    if ((uint8_t)c == 0x89u) { ipc_cycle_focus(); continue; }
                    /* Snap keys: a Wayland app can still be the active window
                     * (sticky kbd focus) with the pointer elsewhere — try it
                     * first, then fall through to IPC/built-in windows. */
                    if ((uint8_t)c == 0x98u) { if (!wayland_snap_focused(1)) ipc_snap_focused(1); continue; }
                    if ((uint8_t)c == 0x99u) { if (!wayland_snap_focused(2)) ipc_snap_focused(2); continue; }
                    if ((uint8_t)c == 0x9Au) { if (!wayland_snap_focused(3)) ipc_snap_focused(3); continue; }
                    if ((uint8_t)c == 0x9Bu) { if (!wayland_snap_focused(0)) ipc_snap_focused(0); continue; }
                    if ((uint8_t)c == 0x9Cu) { compositor_lock(); continue; }
                    if ((uint8_t)c == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                    if ((uint8_t)c == 0x87u) { gui_term_scroll_page(+1); continue; }
                    if ((uint8_t)c == 0x88u) { gui_term_scroll_page(-1); continue; }
                    if ((uint8_t)c == 0x9Eu || (uint8_t)c == 0x9Fu) { continue; }  /* Super tap/help — GUI ring handles; keep out of the PTY */
                    pty_write_input((uint8_t)c);
                }
            } else if (!keyboard_gui_capture_active() && wayland_has_focus()) {
                /* Drain FiFi char queue when Wayland has focus (keys go to browser only) */
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    /* Only allow screenshot/lock/snap shortcuts, not app launchers */
                    if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                    if ((uint8_t)c == 0x9Cu) { compositor_lock(); continue; }
                    if ((uint8_t)c == 0x98u) { if (!wayland_snap_focused(1)) ipc_snap_focused(1); continue; }
                    if ((uint8_t)c == 0x99u) { if (!wayland_snap_focused(2)) ipc_snap_focused(2); continue; }
                    if ((uint8_t)c == 0x9Au) { if (!wayland_snap_focused(3)) ipc_snap_focused(3); continue; }
                    if ((uint8_t)c == 0x9Bu) { if (!wayland_snap_focused(0)) ipc_snap_focused(0); continue; }
                    if ((uint8_t)c == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                    /* Discard everything else — raw keys already forwarded to Wayland */
                }
            } else {
                int c;
                while ((c = keyboard_try_getchar()) != -1) {
                    clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                    if (g_blanked) { g_blanked = false; break; }
                    if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                    if ((uint8_t)c == 0x97u) { close_focused_window(); continue; }
                    if ((uint8_t)c == 0x89u) { ipc_cycle_focus(); continue; }
                    if ((uint8_t)c == 0x98u) { if (!wayland_snap_focused(1)) ipc_snap_focused(1); continue; }
                    if ((uint8_t)c == 0x99u) { if (!wayland_snap_focused(2)) ipc_snap_focused(2); continue; }
                    if ((uint8_t)c == 0x9Au) { if (!wayland_snap_focused(3)) ipc_snap_focused(3); continue; }
                    if ((uint8_t)c == 0x9Bu) { if (!wayland_snap_focused(0)) ipc_snap_focused(0); continue; }
                    if ((uint8_t)c == 0x9Cu) { compositor_lock(); continue; }
                    if ((uint8_t)c == 0x9Du) { if (gui_show_desktop) gui_show_desktop(); continue; }
                }
            }
        }
        keyboard_done:;

        /* ── Wayland key forwarding (raw evdev codes to focused surface) ── */
        {
            extern int keyboard_try_get_raw(uint16_t *code, uint8_t *state);
            uint16_t raw_code; uint8_t raw_state;
            if (wayland_has_focus()) {
                while (keyboard_try_get_raw(&raw_code, &raw_state))
                    wayland_send_key(raw_code, raw_state);
            } else {
                /* Drain queue even when no Wayland focus to prevent buildup */
                while (keyboard_try_get_raw(&raw_code, &raw_state)) {}
            }
        }

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
    pthread_join(pointer_tid, NULL);
    pthread_join(render_tid, NULL);
    return 0;
}
