/* SDL2 input backend — replaces platform/linux/input.c for native host builds */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "mouse.h"

#define FIFI_KEY_LEFT   0x80u
#define FIFI_KEY_RIGHT  0x81u
#define FIFI_KEY_UP     0x82u
#define FIFI_KEY_DOWN   0x83u
#define FIFI_KEY_DELETE 0x84u
#define FIFI_KEY_HOME   0x85u
#define FIFI_KEY_END    0x86u
#define FIFI_KEY_PGUP   0x87u
#define FIFI_KEY_PGDN   0x88u
#define FIFI_KEY_ALTTAB 0x89u
#define FIFI_KEY_F1     0x8Au
#define FIFI_KEY_F2     0x8Bu
#define FIFI_KEY_F3     0x8Cu
#define FIFI_KEY_F4     0x8Du
#define FIFI_KEY_F5     0x8Eu
#define FIFI_KEY_F6     0x8Fu
#define FIFI_KEY_F7     0x90u
#define FIFI_KEY_F8     0x91u
#define FIFI_KEY_F9     0x92u
#define FIFI_KEY_F10    0x93u
#define FIFI_KEY_F11    0x94u
#define FIFI_KEY_F12    0x95u

/* ── Ring buffers ─────────────────────────────────────────────────────────── */
#define KB_RING  256
#define GUI_RING 256

static uint8_t  g_kb_ring[KB_RING];
static uint32_t g_kb_head = 0, g_kb_used = 0;
static bool     g_gui_capture = false;
static uint8_t  g_gui_ring[GUI_RING];
static uint32_t g_gui_head = 0, g_gui_used = 0;

static bool g_shift = false, g_ctrl = false, g_alt = false;

/* ── Mouse ────────────────────────────────────────────────────────────────── */
static int32_t g_mx = 640, g_my = 360;
static bool    g_lbtn = false, g_rbtn = false;
static int32_t g_fb_w = 1280, g_fb_h = 720;

#define CLK_RING 16
typedef struct { int32_t x, y; } click_t;
static click_t  g_clk_ring[CLK_RING];
static uint32_t g_clk_head = 0, g_clk_used = 0;
static int8_t   g_scroll_pending = 0;

/* ── Framebuffer / cursor ─────────────────────────────────────────────────── */
static uint32_t *g_fb_ptr   = NULL;
static uint64_t  g_fb_pitch = 0;

#define CUR_W 12
#define CUR_H 20
static uint32_t g_cur_saved[CUR_W * CUR_H];
static int32_t  g_cur_saved_x = -1, g_cur_saved_y = -1;

static const uint8_t s_cursor[CUR_H][CUR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,1,1,1,1,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,1,0,0,0},
    {0,0,0,0,0,0,0,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
};

void mouse_cursor_update(void) {
    if (!g_fb_ptr) return;
    if (g_cur_saved_x >= 0) {
        for (int cy = 0; cy < CUR_H; cy++) {
            int py = g_cur_saved_y + cy;
            if (py < 0 || py >= g_fb_h) continue;
            for (int cx = 0; cx < CUR_W; cx++) {
                int px = g_cur_saved_x + cx;
                if (px < 0 || px >= g_fb_w) continue;
                g_fb_ptr[(uint64_t)py * g_fb_pitch + px] = g_cur_saved[cy * CUR_W + cx];
            }
        }
    }
    g_cur_saved_x = g_mx; g_cur_saved_y = g_my;
    for (int cy = 0; cy < CUR_H; cy++) {
        int py = g_my + cy; if (py < 0 || py >= g_fb_h) continue;
        for (int cx = 0; cx < CUR_W; cx++) {
            int px = g_mx + cx; if (px < 0 || px >= g_fb_w) continue;
            g_cur_saved[cy * CUR_W + cx] = g_fb_ptr[(uint64_t)py * g_fb_pitch + px];
        }
    }
    for (int cy = 0; cy < CUR_H; cy++) {
        int py = g_my + cy; if (py < 0 || py >= g_fb_h) continue;
        for (int cx = 0; cx < CUR_W; cx++) {
            int px = g_mx + cx; if (px < 0 || px >= g_fb_w) continue;
            uint8_t v = s_cursor[cy][cx];
            if (!v) continue;
            g_fb_ptr[(uint64_t)py * g_fb_pitch + px] = (v == 1) ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
}

/* ── Push helpers ─────────────────────────────────────────────────────────── */
static void kb_push_gui_only(uint8_t c) {
    if (g_gui_used < GUI_RING)
        g_gui_ring[(g_gui_head + g_gui_used++) % GUI_RING] = c;
}

static void kb_push(uint8_t c) {
    if (!c) return;
    if (g_gui_capture) {
        if (g_gui_used < GUI_RING)
            g_gui_ring[(g_gui_head + g_gui_used++) % GUI_RING] = c;
    }
    if (g_kb_used < KB_RING)
        g_kb_ring[(g_kb_head + g_kb_used++) % KB_RING] = c;
}

/* ── SDL event handlers (called from main_sdl.c) ─────────────────────────── */

void input_sdl_handle_textinput(const char *text) {
    if (g_ctrl) return;
    for (int i = 0; text[i] && !(text[i] & 0x80); i++)
        kb_push((uint8_t)text[i]);
}

void input_sdl_handle_keydown(SDL_Keysym ks) {
    SDL_Keymod mod = ks.mod;
    g_shift = (mod & KMOD_SHIFT) != 0;
    g_ctrl  = (mod & KMOD_CTRL)  != 0;
    g_alt   = (mod & KMOD_ALT)   != 0;

    SDL_Keycode sym = ks.sym;

    /* F-keys always go to GUI ring so they work regardless of terminal focus */
    switch (sym) {
        case SDLK_F1:  kb_push_gui_only(FIFI_KEY_F1);  return;
        case SDLK_F2:  kb_push_gui_only(FIFI_KEY_F2);  return;
        case SDLK_F3:  kb_push_gui_only(FIFI_KEY_F3);  return;
        case SDLK_F4:  kb_push_gui_only(FIFI_KEY_F4);  return;
        case SDLK_F5:  kb_push_gui_only(FIFI_KEY_F5);  return;
        case SDLK_F6:  kb_push_gui_only(FIFI_KEY_F6);  return;
        case SDLK_F7:  kb_push_gui_only(FIFI_KEY_F7);  return;
        case SDLK_F8:  kb_push_gui_only(FIFI_KEY_F8);  return;
        case SDLK_F9:  kb_push_gui_only(FIFI_KEY_F9);  return;
        case SDLK_F10: kb_push_gui_only(FIFI_KEY_F10); return;
        case SDLK_F11: kb_push_gui_only(FIFI_KEY_F11); return;
        case SDLK_F12: kb_push_gui_only(FIFI_KEY_F12); return;
    }

    /* Alt+Tab → GUI */
    if (g_alt && sym == SDLK_TAB) { kb_push_gui_only(FIFI_KEY_ALTTAB); return; }

    /* Navigation keys */
    switch (sym) {
        case SDLK_LEFT:      kb_push(FIFI_KEY_LEFT);   return;
        case SDLK_RIGHT:     kb_push(FIFI_KEY_RIGHT);  return;
        case SDLK_UP:        kb_push(FIFI_KEY_UP);     return;
        case SDLK_DOWN:      kb_push(FIFI_KEY_DOWN);   return;
        case SDLK_DELETE:    kb_push(FIFI_KEY_DELETE); return;
        case SDLK_HOME:      kb_push(FIFI_KEY_HOME);   return;
        case SDLK_END:       kb_push(FIFI_KEY_END);    return;
        case SDLK_PAGEUP:    kb_push(FIFI_KEY_PGUP);   return;
        case SDLK_PAGEDOWN:  kb_push(FIFI_KEY_PGDN);   return;
        case SDLK_BACKSPACE: kb_push('\b');             return;
        case SDLK_TAB:       kb_push('\t');             return;
        case SDLK_RETURN:    kb_push('\n');             return;
        case SDLK_ESCAPE:    kb_push(0x1Bu);            return;
    }

    /* Ctrl+letter → control codes matching the evdev driver mapping */
    if (g_ctrl && sym >= 'a' && sym <= 'z') {
        uint8_t cc = 0;
        switch (sym) {
            case 'a': cc =  1; break;  case 'b': cc =  2; break;
            case 'c': cc =  3; break;  case 'd': cc =  4; break;
            case 'e': cc =  5; break;  case 'f': cc =  6; break;
            case 'k': cc = 11; break;  case 'n': cc = 14; break;
            case 'p': cc = 16; break;  case 'r': cc = 18; break;
            case 's': cc = 19; break;  case 't': cc = 20; break;
            case 'u': cc = 21; break;  case 'v': cc = 22; break;
            case 'w': cc = 23; break;  case 'x': cc = 24; break;
            case 'y': cc = 25; break;  case 'z': cc = 26; break;
        }
        if (cc) { kb_push(cc); return; }
        kb_push((uint8_t)sym);
        return;
    }

    /* Ctrl+non-letter: push the char, gui.c reads kbd_ctrl_down() separately */
    if (g_ctrl && sym > 0 && sym < 128) {
        kb_push((uint8_t)sym);
        return;
    }

    /* Printable chars without Ctrl are handled by SDL_TEXTINPUT */
}

void input_sdl_handle_keyup(SDL_Keysym ks) {
    SDL_Keymod mod = ks.mod;
    g_shift = (mod & KMOD_SHIFT) != 0;
    g_ctrl  = (mod & KMOD_CTRL)  != 0;
    g_alt   = (mod & KMOD_ALT)   != 0;
}

void input_sdl_handle_mousepos(int32_t x, int32_t y) {
    g_mx = x < 0 ? 0 : x >= g_fb_w ? g_fb_w - 1 : x;
    g_my = y < 0 ? 0 : y >= g_fb_h ? g_fb_h - 1 : y;
}

void input_sdl_handle_mousebtn(uint8_t btn, bool down, int32_t x, int32_t y) {
    input_sdl_handle_mousepos(x, y);
    bool prev_l = g_lbtn;
    if (btn == SDL_BUTTON_LEFT)  g_lbtn = down;
    if (btn == SDL_BUTTON_RIGHT) g_rbtn = down;
    if (g_lbtn && !prev_l && g_clk_used < CLK_RING)
        g_clk_ring[(g_clk_head + g_clk_used++) % CLK_RING] = (click_t){g_mx, g_my};
}

void input_sdl_handle_wheel(int32_t dy) {
    if (dy) g_scroll_pending = dy > 0 ? 1 : -1;
}

void input_sdl_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h) {
    g_fb_ptr = ptr; g_fb_pitch = pitch32; g_fb_w = w; g_fb_h = h;
}

/* ── Public API (same signatures as platform/linux/input.c) ──────────────── */

void  input_init(void)  { }
void  input_poll(void)  { }
int   input_get_all_fds(int *buf, int maxn) { (void)buf; (void)maxn; return 0; }
void  input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h)
    { input_sdl_set_fb(ptr, pitch32, w, h); }

void  mouse_init(void)  { g_mx = g_fb_w / 2; g_my = g_fb_h / 2; }
void  mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn) {
    g_mx += dx; g_my += dy;
    g_mx = g_mx < 0 ? 0 : g_mx >= g_fb_w ? g_fb_w-1 : g_mx;
    g_my = g_my < 0 ? 0 : g_my >= g_fb_h ? g_fb_h-1 : g_my;
    g_lbtn = lbtn; g_rbtn = rbtn;
}
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn) {
    if (x) *x=g_mx; if (y) *y=g_my;
    if (lbtn) *lbtn=g_lbtn; if (rbtn) *rbtn=g_rbtn;
}
bool  mouse_consume_click(int32_t *x, int32_t *y) {
    if (!g_clk_used) return false;
    click_t c = g_clk_ring[g_clk_head];
    g_clk_head=(g_clk_head+1)%CLK_RING; g_clk_used--;
    if (x) *x=c.x; if (y) *y=c.y;
    return true;
}
int8_t mouse_consume_scroll(void)  { int8_t v=g_scroll_pending; g_scroll_pending=0; return v; }
void   mouse_warp(int32_t x, int32_t y)  { g_mx=x; g_my=y; }
void   mouse_click(int32_t x, int32_t y) {
    if (g_clk_used < CLK_RING)
        g_clk_ring[(g_clk_head+g_clk_used++)%CLK_RING]=(click_t){x,y};
}
void   mouse_irq_handler(void)             { }
void   mouse_on_byte(uint8_t b)            { (void)b; }
void   mouse_set_intellimouse(bool e)      { (void)e; }
static cursor_type_t g_cur_type = CURSOR_ARROW;
void   mouse_set_cursor(cursor_type_t t)   { g_cur_type=t; }
cursor_type_t mouse_get_cursor(void)       { return g_cur_type; }

void  keyboard_push_char(uint8_t c)        { kb_push(c); }
int   keyboard_try_getchar(void) {
    if (!g_kb_used) return -1;
    uint8_t c=g_kb_ring[g_kb_head]; g_kb_head=(g_kb_head+1)%KB_RING; g_kb_used--;
    return (int)(unsigned int)c;
}
void  keyboard_set_gui_capture(bool on)    { g_gui_capture=on; g_kb_used=g_gui_used=0; }
int   keyboard_gui_try_getchar(void) {
    if (!g_gui_used) return -1;
    uint8_t c=g_gui_ring[g_gui_head]; g_gui_head=(g_gui_head+1)%GUI_RING; g_gui_used--;
    return (int)(unsigned int)c;
}
bool  keyboard_gui_capture_active(void)    { return g_gui_capture; }
bool  kbd_shift_down(void)                 { return g_shift; }
bool  kbd_ctrl_down(void)                  { return g_ctrl; }
bool  kbd_alt_down(void)                   { return g_alt; }

void     keyboard_on_scancode(uint8_t sc)           { (void)sc; }
uint64_t keyboard_irq_count(void)                   { return 0; }
void     keyboard_irq_handler(void)                 { }
void     keyboard_ps2_init(void)                    { }
void     keyboard_ps2_poll(void)                    { }
void     keyboard_repeat_tick(void)                 { }
void     keyboard_ps2_diag(void)                    { }
void     keyboard_ps2_full_init(void)               { }
uint32_t keyboard_sc_make(uint8_t sc)               { (void)sc; return 0; }
uint32_t keyboard_sc_break(uint8_t sc)              { (void)sc; return 0; }
void     keyboard_clear_state(void)                 { g_kb_used=g_gui_used=0; }
void     keyboard_hid_make(uint8_t kc, uint8_t ch)  { (void)kc; kb_push(ch); }
void     keyboard_hid_break(uint8_t kc)             { (void)kc; }
void     keyboard_set_hid_present(void)             { }
void     keyboard_set_raw_capture(int on)           { (void)on; }
uint32_t keyboard_raw_total(void)                   { return 0; }
uint32_t keyboard_raw_aux(void)                     { return 0; }
int      keyboard_has_data(void)                    { return g_kb_used > 0 ? 1 : 0; }
