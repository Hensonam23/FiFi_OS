/* FiFi OS — native SDL2 host runner
 * Runs the FiFi compositor directly as a desktop window — no QEMU, no fb0.
 * Display syncs to monitor VSync via SDL_RENDERER_PRESENTVSYNC. */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>

#include "console.h"
#include "gui.h"
#include "limine.h"

#ifndef SDL_WIN_W
#define SDL_WIN_W 1280
#endif
#ifndef SDL_WIN_H
#define SDL_WIN_H 720
#endif

/* Platform functions (from platform/linux/) */
void pit_init(uint32_t hz);
void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm);
void vfs_init(void);
void pty_init(void);
void pty_poll_output(void);
void pty_write_input(uint8_t c);
void pty_set_winsize(uint16_t cols, uint16_t rows);

/* SDL input functions (from input_sdl.c) */
void input_sdl_handle_textinput(const char *text);
void input_sdl_handle_keydown(SDL_Keysym ks);
void input_sdl_handle_keyup(SDL_Keysym ks);
void input_sdl_handle_mousepos(int32_t x, int32_t y);
void input_sdl_handle_mousebtn(uint8_t btn, bool down, int32_t x, int32_t y);
void input_sdl_handle_wheel(int32_t dy);
void input_sdl_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h);

bool  keyboard_gui_capture_active(void);
int   keyboard_try_getchar(void);
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);
void  mouse_cursor_update(void);
void  mouse_init(void);

static SDL_Window   *g_win    = NULL;
static SDL_Renderer *g_ren    = NULL;
static SDL_Texture  *g_tex    = NULL;
static uint32_t     *g_pixels = NULL;

static struct limine_framebuffer g_lmfb;

static void on_signal(int s) { (void)s; SDL_Quit(); _exit(0); }

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "[fifi-sdl] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    g_win = SDL_CreateWindow(
        "FiFi OS — linux-desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SDL_WIN_W, SDL_WIN_H, 0);
    if (!g_win) {
        fprintf(stderr, "[fifi-sdl] CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    /* Hardware-accelerated renderer with VSync — display syncs to monitor refresh */
    g_ren = SDL_CreateRenderer(g_win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) {
        fprintf(stderr, "[fifi-sdl] hw renderer failed (%s), trying software\n",
                SDL_GetError());
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_ren) {
        fprintf(stderr, "[fifi-sdl] CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, SDL_WIN_W, SDL_WIN_H);
    if (!g_tex) {
        fprintf(stderr, "[fifi-sdl] CreateTexture: %s\n", SDL_GetError());
        return 1;
    }

    g_pixels = (uint32_t *)calloc((size_t)SDL_WIN_W * SDL_WIN_H, sizeof(uint32_t));
    if (!g_pixels) { fprintf(stderr, "[fifi-sdl] OOM\n"); return 1; }

    g_lmfb.address = g_pixels;
    g_lmfb.width   = SDL_WIN_W;
    g_lmfb.height  = SDL_WIN_H;
    g_lmfb.pitch   = (uint64_t)SDL_WIN_W * sizeof(uint32_t);
    g_lmfb.bpp     = 32;

    pit_init(100);
    pmm_init(NULL, 0);
    vfs_init();

    { extern void net_init(void); net_init(); }

    console_init(&g_lmfb);
    console_backbuf_init();

    input_sdl_set_fb(g_pixels, SDL_WIN_W, SDL_WIN_W, SDL_WIN_H);
    mouse_init();

    pty_init();
    {
        uint32_t fw = console_font_width(), fh = console_font_height();
        if (fw > 0 && fh > 0) {
            uint16_t cols = (uint16_t)((SDL_WIN_W > 10u ? SDL_WIN_W - 10u : 1u) / fw);
            uint16_t rows = (uint16_t)((SDL_WIN_H > 85u ? SDL_WIN_H - 85u : 1u) / fh);
            if (cols < 20) cols = 20;
            if (rows < 5)  rows = 5;
            pty_set_winsize(cols, rows);
            fprintf(stderr, "[fifi-sdl] terminal %ux%u chars\n", cols, rows);
        }
    }

    gui_init();
    mouse_cursor_update();

    SDL_StartTextInput();
    SDL_SetRelativeMouseMode(SDL_FALSE);

    fprintf(stderr, "[fifi-sdl] running %dx%d (VSync)\n", SDL_WIN_W, SDL_WIN_H);

    int32_t last_cx = -1, last_cy = -1;

    for (;;) {
        /* ── Drain SDL events ──────────────────────────────────────────────── */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    SDL_Quit();
                    return 0;
                case SDL_TEXTINPUT:
                    input_sdl_handle_textinput(ev.text.text);
                    break;
                case SDL_KEYDOWN:
                    input_sdl_handle_keydown(ev.key.keysym);
                    break;
                case SDL_KEYUP:
                    input_sdl_handle_keyup(ev.key.keysym);
                    break;
                case SDL_MOUSEMOTION:
                    input_sdl_handle_mousepos(ev.motion.x, ev.motion.y);
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    input_sdl_handle_mousebtn(ev.button.button, true,
                                              ev.button.x, ev.button.y);
                    break;
                case SDL_MOUSEBUTTONUP:
                    input_sdl_handle_mousebtn(ev.button.button, false,
                                              ev.button.x, ev.button.y);
                    break;
                case SDL_MOUSEWHEEL:
                    input_sdl_handle_wheel(ev.wheel.y);
                    break;
            }
        }

        /* ── PTY output → terminal ─────────────────────────────────────────── */
        pty_poll_output();

        /* ── Keyboard → PTY when terminal focused ──────────────────────────── */
        if (!keyboard_gui_capture_active()) {
            int c;
            while ((c = keyboard_try_getchar()) != -1)
                pty_write_input((uint8_t)c);
        }

        /* ── GUI tick ──────────────────────────────────────────────────────── */
        gui_on_tick();

        /* ── Flip dirty backbuffer rows → pixel buffer ─────────────────────── */
        console_flip_if_dirty();

        /* ── Cursor: update only if moved ──────────────────────────────────── */
        int32_t cx, cy; bool lb, rb;
        mouse_get_state(&cx, &cy, &lb, &rb);
        if (cx != last_cx || cy != last_cy) {
            mouse_cursor_update();
            last_cx = cx; last_cy = cy;
        }

        /* ── Upload pixel buffer → GPU texture → present (VSync) ────────────── */
        void  *tex_px; int tex_pitch;
        SDL_LockTexture(g_tex, NULL, &tex_px, &tex_pitch);
        if (tex_pitch == SDL_WIN_W * 4) {
            memcpy(tex_px, g_pixels, (size_t)SDL_WIN_W * SDL_WIN_H * 4);
        } else {
            for (int y = 0; y < SDL_WIN_H; y++)
                memcpy((uint8_t *)tex_px + y * tex_pitch,
                       g_pixels + y * SDL_WIN_W,
                       (size_t)SDL_WIN_W * 4);
        }
        SDL_UnlockTexture(g_tex);

        SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
        SDL_RenderPresent(g_ren);   /* blocks until VSync — smooth 60fps */
    }
}
