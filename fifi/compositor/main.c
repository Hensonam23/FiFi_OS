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

/* Input query functions */
bool  keyboard_gui_capture_active(void);
int   input_get_all_fds(int *buf, int maxn);
int   keyboard_try_getchar(void);
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);

/* ── Framebuffer setup ───────────────────────────────────────────────────── */

static int      g_fb_fd   = -1;
static uint32_t *g_fb_mem = NULL;
static size_t   g_fb_size = 0;
static struct   limine_framebuffer g_lmfb;
static bool     g_using_drm  = false;
static bool     g_gaming_mode = false;  /* set by GUI; removes poll cap */
static uint32_t g_fps_current = 0;      /* last measured frame rate */
static bool     g_blanked     = false;  /* display blanked due to inactivity */
static struct timespec g_last_input;    /* time of last user input */
#define BLANK_TIMEOUT_S 300             /* blank after 5 minutes of no input */

bool gaming_mode_active(void)   { return g_gaming_mode; }
uint32_t compositor_fps(void)   { return g_fps_current; }
void gaming_mode_set(bool on)   {
    g_gaming_mode = on;
    fprintf(stderr, "[compositor] gaming mode %s\n", on ? "ON" : "OFF");
    /* Hint CPU governor — best-effort, non-fatal */
    const char *gov = on ? "performance" : "schedutil";
    for (int cpu = 0; cpu < 8; cpu++) {
        char path[80];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        int fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, gov, strlen(gov)); close(fd); }
    }
}

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

    /* Try DRM/KMS first (virtio-gpu: triggers explicit flush → instant QEMU update).
     * Fall back to /dev/fb0 if DRM isn't available (e.g. virtio-vga or bare fb). */
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

    /* net_init detects NICs from /proc/net/dev */
    extern void net_init(void);
    net_init();

    /* ALSA volume control — non-fatal if audio device not present */
    extern bool hda_init(void);
    hda_init();

    console_init(&g_lmfb);
    console_backbuf_init();

    input_set_fb(g_lmfb.address, (uint64_t)(g_lmfb.pitch / 4),
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

    /* IPC socket — apps connect here to create windows and push frames */
    ipc_init();

    /* Wayland compositor socket — native Wayland clients connect here */
    wayland_set_display_size((int)g_lmfb.width, (int)g_lmfb.height);
    if (!wayland_init())
        fprintf(stderr, "[compositor] Wayland init failed — continuing without it\n");

    fprintf(stderr, "[compositor] running\n");

    /* Gather evdev fds for poll() */
    int evdev_fds[20];
    int nevdev = input_get_all_fds(evdev_fds, 20);

#define MAX_PFD 24
    struct pollfd pfd[MAX_PFD];

    /* FPS tracking */
    struct timespec fps_ts;
    clock_gettime(CLOCK_MONOTONIC, &fps_ts);
    uint32_t fps_frames = 0;

    /* Screen-blank inactivity timer */
    clock_gettime(CLOCK_MONOTONIC, &g_last_input);

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

        /* Include IPC server fd so new app connections wake us immediately */
        int ipc_fd = ipc_server_fd();
        if (ipc_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = ipc_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        /* Include Wayland server fd */
        int wl_fd = wayland_server_fd();
        if (wl_fd >= 0 && nfds < MAX_PFD) {
            pfd[nfds].fd     = wl_fd;
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        /* 4 ms cap (250 Hz) normally; 0 in gaming mode for max frame rate */
        poll(pfd, (nfds_t)nfds, g_gaming_mode ? 0 : 4);

        /* ── IPC: accept new app connections, read app messages ──────────── */
        ipc_poll();
        wayland_poll();

        /* ── Gamepad → IPC focused app ───────────────────────────────────── */
        if (ipc_keyboard_active() && input_gamepad_connected()) {
            for (int gi = 0; gi < 2; gi++) {
                if (!input_gamepad_changed(gi)) continue;
                uint16_t btns; int16_t lx, ly, rx, ry, lt, rt;
                if (input_gamepad_state(gi, &btns, &lx, &ly, &rx, &ry, &lt, &rt))
                    ipc_send_gamepad(btns, lx, ly, rx, ry, lt, rt);
            }
        }

        /* ── PTY output → console buffer ───────────────────────────────── */
        pty_poll_output();

        /* ── evdev events ──────────────────────────────────────────────── */
        bool had_input_before = false;
        {
            /* Snapshot mouse state before polling so we can detect changes */
            int32_t px, py; bool pb_l, pb_r;
            mouse_get_state(&px, &py, &pb_l, &pb_r);
            input_poll();
            int32_t nx, ny; bool nb_l, nb_r;
            mouse_get_state(&nx, &ny, &nb_l, &nb_r);
            had_input_before = (nx != px || ny != py || nb_l != pb_l || nb_r != pb_r);
        }

        /* ── Input routing: IPC app > PTY > GUI ────────────────────────
         * Check mouse first so we can update IPC focus before routing keys. */
        {
            int32_t mcx, mcy; bool mlb, mrb;
            mouse_get_state(&mcx, &mcy, &mlb, &mrb);
            uint8_t btns = (mlb ? 1 : 0) | (mrb ? 2 : 0);

            /* Update resize (must check before drag to avoid conflict) */
            /* File drag in progress: update cursor and handle release */
            if (ipc_file_drag_active()) {
                ipc_file_drag_update(mcx, mcy);
                if (!mlb) ipc_file_drag_drop(mcx, mcy);
                goto mouse_done;
            }

            bool resizing = ipc_resize_update(mcx, mcy, mlb);

            /* Update drag (move IPC window) — do this before hit-test */
            bool dragging = !resizing && ipc_drag_update(mcx, mcy, mlb);

            if (mlb && !dragging && !resizing) {
                /* Close button takes priority over hit-test and drag */
                if (!ipc_try_close_at(mcx, mcy)) {
                    /* Try resize handle first */
                    if (!ipc_resize_begin(mcx, mcy)) {
                        /* On left-click: check if it lands on an IPC window */
                        if (!ipc_hit_test(mcx, mcy))
                            ipc_clear_focus();
                    }
                }
            }
            if (ipc_keyboard_active() && !dragging && !resizing)
                ipc_send_focused_mouse(mcx, mcy, btns);
            mouse_done:;

            /* Forward mouse to Wayland surfaces (when no IPC window has focus) */
            if (!ipc_keyboard_active())
                wayland_send_mouse(mcx, mcy, btns);
        }

        /* ── Activity tracking: mouse movement resets the blank timer ───── */
        if (had_input_before)
            clock_gettime(CLOCK_MONOTONIC, &g_last_input);

        if (ipc_keyboard_active()) {
            /* Keys go to the focused IPC app, except F1-F4 which always reach the GUI */
            int c;
            while ((c = keyboard_try_getchar()) != -1) {
                clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                if (g_blanked) { g_blanked = false; break; }  /* first key wakes display */
                uint8_t uc = (uint8_t)c;
                if (uc == 0x96u) { take_screenshot(); continue; }
                if (uc == 0x1Bu && ipc_file_drag_active()) { ipc_file_drag_cancel(); continue; }
                if (uc >= 0x8Au && uc <= 0x8Du) {
                    extern void keyboard_push_char(uint8_t c);
                    keyboard_push_char(uc);
                } else {
                    ipc_send_focused_key(uc);
                }
            }
        } else if (!keyboard_gui_capture_active()) {
            /* Terminal is focused — keys go to PTY */
            int c;
            while ((c = keyboard_try_getchar()) != -1) {
                clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                if (g_blanked) { g_blanked = false; break; }  /* first key wakes display */
                if ((uint8_t)c == 0x96u) { take_screenshot(); continue; }
                pty_write_input((uint8_t)c);
            }
        } else {
            /* GUI capture active — drain kb_ring to prevent overflow; intercept PrtSc.
             * gui_on_tick() reads its own gui_ring via keyboard_gui_try_getchar(). */
            int c;
            while ((c = keyboard_try_getchar()) != -1) {
                clock_gettime(CLOCK_MONOTONIC, &g_last_input);
                if (g_blanked) { g_blanked = false; break; }
                if ((uint8_t)c == 0x96u) take_screenshot();
            }
        }

        /* Wake display on mouse input when blanked */
        if (g_blanked && had_input_before) {
            g_blanked = false;
            clock_gettime(CLOCK_MONOTONIC, &g_last_input);
        }

        /* ── Screen blank check ─────────────────────────────────────────── */
        {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long idle_s = now_ts.tv_sec - g_last_input.tv_sec;
            if (!g_blanked && !g_gaming_mode && idle_s >= BLANK_TIMEOUT_S) {
                g_blanked = true;
                if (g_using_drm) drm_blank_display();
                else if (g_fb_mem) memset(g_fb_mem, 0, g_fb_size);
                fprintf(stderr, "[compositor] display blanked (idle %lds)\n", idle_s);
            }
        }

        /* ── GUI tick (always runs — clock/state must update even when blanked) */
        gui_on_tick();

        /* ── IPC windows: blit cached frames ON TOP of GUI background ─────── */
        ipc_blit_all();
        wayland_blit_surfaces();

        /* ── Cursor erase: mark old cursor rows dirty so the flip below
         * pulls clean backbuffer content there (erasing the stale cursor). ── */
        int32_t cx, cy; bool lb, rb;
        mouse_get_state(&cx, &cy, &lb, &rb);
        static int32_t s_last_cx = -1, s_last_cy = -1;
        bool cursor_moved = (cx != s_last_cx || cy != s_last_cy);

        if (!g_blanked) {
            if (cursor_moved && s_last_cy >= 0) {
                uint32_t ey0 = (uint32_t)(s_last_cy < 0 ? 0 : s_last_cy);
                uint32_t ey1 = ey0 + CUR_H;
                console_mark_dirty_rows(ey0, ey1);
            }

            /* ── IPC overlays (title bars, close/min buttons, resize handles) */
            ipc_draw_overlays();
            ipc_draw_resize_handles();
            ipc_draw_drag_overlay();
            ipc_notify_draw();

            /* ── Flip dirty rows (backbuf → frontbuf), then push to QEMU ── */
            bool flipped = console_flip_if_dirty();

            /* ── Cursor redraw: save from backbuf (stale-proof), draw on front */
            if (flipped || cursor_moved) {
                mouse_cursor_update();
                s_last_cx = cx; s_last_cy = cy;
            }

            /* ── Single DRM flush covers both content and cursor ─────────── */
            if ((flipped || cursor_moved) && g_using_drm) drm_flush();
        } else {
            /* Blanked: keep frontbuffer black, but track cursor for wake-up */
            s_last_cx = cx; s_last_cy = cy;
        }

        /* ── FPS counter: update once per second ────────────────────────── */
        fps_frames++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - fps_ts.tv_sec) * 1000L
                        + (now.tv_nsec - fps_ts.tv_nsec) / 1000000L;
        if (elapsed_ms >= 1000) {
            g_fps_current = (uint32_t)(fps_frames * 1000u / (uint32_t)elapsed_ms);
            fps_frames = 0;
            fps_ts = now;
        }
    }

    return 0;
}
