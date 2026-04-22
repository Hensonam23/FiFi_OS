#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>

/* Include mouse.h BEFORE linux/input.h — mouse.h has no KEY_* conflicts */
#include "mouse.h"

/* linux/input.h defines its own KEY_LEFT=105, KEY_F1=59, etc.
 * We keep this include isolated: after mouse.h, before keyboard.h.
 * The FiFi keyboard KEY_* (0x80-0x95) are defined locally as FIFI_KEY_*. */
#include <linux/input.h>

/* ── FiFi special key codes — mirror of kernel/include/keyboard.h (0x80-0x95) */
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

/* ── Evdev device fds ─────────────────────────────────────────────────────── */
#define MAX_EVDEV 8
static int g_kbd_fds[MAX_EVDEV];
static int g_kbd_cnt = 0;
static int g_ptr_fds[MAX_EVDEV];   /* relative mice */
static int g_ptr_cnt = 0;
typedef struct { int fd; int32_t x_max, y_max; } abs_dev_t;
static abs_dev_t g_abs_devs[MAX_EVDEV];
static int g_abs_cnt = 0;

/* ── Keyboard state ───────────────────────────────────────────────────────── */
#define KB_RING  256
#define GUI_RING 256

static uint8_t  g_kb_ring[KB_RING];
static uint32_t g_kb_head = 0;
static uint32_t g_kb_used = 0;

static bool     g_gui_capture = false;
static uint8_t  g_gui_ring[GUI_RING];
static uint32_t g_gui_head = 0;
static uint32_t g_gui_used = 0;

static bool g_shift = false;
static bool g_ctrl  = false;
static bool g_alt   = false;

/* ── Mouse state ─────────────────────────────────────────────────────────── */
static int32_t g_mx = 400, g_my = 300;
static bool    g_lbtn = false, g_rbtn = false;
static int32_t g_fb_w = 1024, g_fb_h = 768;

#define CLK_RING 16
typedef struct { int32_t x, y; } click_t;
static click_t  g_clk_ring[CLK_RING];
static uint32_t g_clk_head = 0;
static uint32_t g_clk_used = 0;
static int8_t   g_scroll_pending = 0;

/* ── Mouse cursor (drawn on real framebuffer after backbuf flip) ─────────── */
static uint32_t *g_fb_ptr   = NULL;
static uint64_t  g_fb_pitch = 0;

#define CUR_W 12
#define CUR_H 20
static uint32_t g_cur_saved[CUR_W * CUR_H];
static int32_t  g_cur_saved_x = -1, g_cur_saved_y = -1;

void input_set_fb(uint32_t *ptr, uint64_t pitch32, int32_t w, int32_t h) {
    g_fb_ptr = ptr; g_fb_pitch = pitch32; g_fb_w = w; g_fb_h = h;
}

/* ── Key translation (linux evdev codes → FiFi chars / FIFI_KEY_*) ────────── */

static uint8_t evkey_to_fifi(uint16_t code, bool shift, bool ctrl) {
    switch (code) {
        /* Navigation */
        case KEY_LEFT:     return FIFI_KEY_LEFT;
        case KEY_RIGHT:    return FIFI_KEY_RIGHT;
        case KEY_UP:       return FIFI_KEY_UP;
        case KEY_DOWN:     return FIFI_KEY_DOWN;
        case KEY_DELETE:   return FIFI_KEY_DELETE;
        case KEY_HOME:     return FIFI_KEY_HOME;
        case KEY_END:      return FIFI_KEY_END;
        case KEY_PAGEUP:   return FIFI_KEY_PGUP;
        case KEY_PAGEDOWN: return FIFI_KEY_PGDN;
        /* Function keys */
        case KEY_F1:  return FIFI_KEY_F1;
        case KEY_F2:  return FIFI_KEY_F2;
        case KEY_F3:  return FIFI_KEY_F3;
        case KEY_F4:  return FIFI_KEY_F4;
        case KEY_F5:  return FIFI_KEY_F5;
        case KEY_F6:  return FIFI_KEY_F6;
        case KEY_F7:  return FIFI_KEY_F7;
        case KEY_F8:  return FIFI_KEY_F8;
        case KEY_F9:  return FIFI_KEY_F9;
        case KEY_F10: return FIFI_KEY_F10;
        case KEY_F11: return FIFI_KEY_F11;
        case KEY_F12: return FIFI_KEY_F12;
        /* ASCII control */
        case KEY_BACKSPACE: return '\b';
        case KEY_TAB:       return '\t';
        case KEY_ENTER:     return '\n';
        case KEY_ESC:       return 0x1Bu;
    }

    /* Ctrl+letter → control code */
    if (ctrl) {
        const char *letters = "aqwertyuiopasdfghjklzxcvbnm";
        /* Map printable key code to letter, then to ctrl code */
        /* (just handle the common ctrl combos explicitly) */
        switch (code) {
            case KEY_C: return 3;   /* ETX */
            case KEY_D: return 4;   /* EOT */
            case KEY_Z: return 26;  /* SUB */
            case KEY_A: return 1;
            case KEY_E: return 5;
            case KEY_U: return 21;
            case KEY_K: return 11;
            case KEY_W: return 23;
            case KEY_B: return 2;
            case KEY_F: return 6;
            case KEY_N: return 14;
            case KEY_P: return 16;
            case KEY_R: return 18;
            case KEY_S: return 19;
            case KEY_T: return 20;
            case KEY_V: return 22;
            case KEY_X: return 24;
            case KEY_Y: return 25;
            default: (void)letters; break;
        }
    }

    /* Printable characters via lookup tables */
    static const char kn[128] = {
        /* 0-1 */  0, 0,
        /* 2  */  '1','2','3','4','5','6','7','8','9','0','-','=',
        /* 14 */   0,  /* BS handled above */
        /* 15 */   0,  /* TAB handled above */
        /* 16 */  'q','w','e','r','t','y','u','i','o','p','[',']',
        /* 28 */   0,  /* ENTER */
        /* 29 */   0,  /* LCTRL */
        /* 30 */  'a','s','d','f','g','h','j','k','l',';','\'','`',
        /* 42 */   0,  /* LSHIFT */
        /* 43 */  '\\',
        /* 44 */  'z','x','c','v','b','n','m',',','.','/',
        /* 54 */   0,0,0,
        /* 57 */  ' ',
        0
    };
    static const char ks[128] = {
        /* 0-1 */  0, 0,
        /* 2  */  '!','@','#','$','%','^','&','*','(',')','_','+',
        /* 14 */   0,
        /* 15 */   0,
        /* 16 */  'Q','W','E','R','T','Y','U','I','O','P','{','}',
        /* 28 */   0,
        /* 29 */   0,
        /* 30 */  'A','S','D','F','G','H','J','K','L',':','"','~',
        /* 42 */   0,
        /* 43 */  '|',
        /* 44 */  'Z','X','C','V','B','N','M','<','>','?',
        /* 54 */   0,0,0,
        /* 57 */  ' ',
        0
    };

    if (code < 58) {
        char c = shift ? ks[code] : kn[code];
        return (uint8_t)c;
    }
    return 0;
}

static void kb_push_internal(uint8_t c) {
    if (!c) return;
    if (g_gui_capture) {
        if (g_gui_used < GUI_RING) {
            g_gui_ring[(g_gui_head + g_gui_used) % GUI_RING] = c;
            g_gui_used++;
        }
    }
    /* Always push to shell ring too (gui_capture only controls which gets priority) */
    if (g_kb_used < KB_RING) {
        g_kb_ring[(g_kb_head + g_kb_used) % KB_RING] = c;
        g_kb_used++;
    }
}

/* ── Software mouse cursor ───────────────────────────────────────────────── */

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

    /* Restore previous area */
    if (g_cur_saved_x >= 0) {
        for (int cy = 0; cy < CUR_H; cy++) {
            int py = g_cur_saved_y + cy;
            if (py < 0 || py >= g_fb_h) continue;
            for (int cx = 0; cx < CUR_W; cx++) {
                int px = g_cur_saved_x + cx;
                if (px < 0 || px >= g_fb_w) continue;
                g_fb_ptr[(uint64_t)py * g_fb_pitch + px] =
                    g_cur_saved[cy * CUR_W + cx];
            }
        }
    }

    /* Save new area */
    g_cur_saved_x = g_mx;
    g_cur_saved_y = g_my;
    for (int cy = 0; cy < CUR_H; cy++) {
        int py = g_my + cy;
        if (py < 0 || py >= g_fb_h) continue;
        for (int cx = 0; cx < CUR_W; cx++) {
            int px = g_mx + cx;
            if (px < 0 || px >= g_fb_w) continue;
            g_cur_saved[cy * CUR_W + cx] =
                g_fb_ptr[(uint64_t)py * g_fb_pitch + px];
        }
    }

    /* Draw cursor */
    for (int cy = 0; cy < CUR_H; cy++) {
        int py = g_my + cy;
        if (py < 0 || py >= g_fb_h) continue;
        for (int cx = 0; cx < CUR_W; cx++) {
            int px = g_mx + cx;
            if (px < 0 || px >= g_fb_w) continue;
            uint8_t v = s_cursor[cy][cx];
            if (v == 0) continue;
            uint32_t col = (v == 1) ? 0x00FFFFFFu : 0x00000000u;
            g_fb_ptr[(uint64_t)py * g_fb_pitch + px] = col;
        }
    }
}

/* ── Evdev device detection ─────────────────────────────────────────────── */

static bool evdev_has_bit(int fd, int type, int bit) {
    uint8_t bits[96] = {0};
    ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits);
    return (bits[bit / 8] >> (bit % 8)) & 1;
}

void input_init(void) {
    DIR *d = opendir("/dev/input");
    if (!d) { fprintf(stderr, "[input] /dev/input not found\n"); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        bool has_key = evdev_has_bit(fd, 0, EV_KEY);
        bool has_rel = evdev_has_bit(fd, 0, EV_REL);

        if (has_key && evdev_has_bit(fd, EV_KEY, KEY_A)) {
            if (g_kbd_cnt < MAX_EVDEV) {
                g_kbd_fds[g_kbd_cnt++] = fd;
                fprintf(stderr, "[input] keyboard: %s\n", path);
                continue;
            }
        }
        /* Check absolute BEFORE relative — virtio-tablet has both EV_ABS and EV_REL */
        bool has_abs = evdev_has_bit(fd, 0, EV_ABS);
        if (has_abs && evdev_has_bit(fd, EV_ABS, ABS_X) &&
            evdev_has_bit(fd, EV_ABS, ABS_Y) &&
            evdev_has_bit(fd, EV_KEY, BTN_LEFT)) {
            if (g_abs_cnt < MAX_EVDEV) {
                struct input_absinfo ai;
                abs_dev_t *dev = &g_abs_devs[g_abs_cnt++];
                dev->fd = fd;
                dev->x_max = (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > 0)
                             ? ai.maximum : 32767;
                dev->y_max = (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > 0)
                             ? ai.maximum : 32767;
                fprintf(stderr, "[input] tablet: %s (range %dx%d)\n",
                        path, dev->x_max, dev->y_max);
                continue;
            }
        }
        if (has_rel && evdev_has_bit(fd, EV_KEY, BTN_LEFT)) {
            if (g_ptr_cnt < MAX_EVDEV) {
                g_ptr_fds[g_ptr_cnt++] = fd;
                fprintf(stderr, "[input] mouse: %s\n", path);
                continue;
            }
        }
        close(fd);
    }
    closedir(d);

    if (g_kbd_cnt == 0) fprintf(stderr, "[input] warning: no keyboard found\n");
    if (g_ptr_cnt == 0 && g_abs_cnt == 0) fprintf(stderr, "[input] warning: no mouse found\n");
}

/* ── Poll — call each frame ─────────────────────────────────────────────── */

void input_poll(void) {
    struct input_event ev;

    for (int ki = 0; ki < g_kbd_cnt; ki++) {
        while (read(g_kbd_fds[ki], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type != EV_KEY) continue;
            bool pressed = (ev.value == 1 || ev.value == 2);

            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
                g_shift = pressed;
            else if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
                g_ctrl = pressed;
            else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
                g_alt = pressed;

            if (!pressed) continue;

            if (g_alt && ev.code == KEY_TAB) {
                kb_push_internal(FIFI_KEY_ALTTAB);
                continue;
            }

            uint8_t c = evkey_to_fifi((uint16_t)ev.code, g_shift, g_ctrl);
            if (c) kb_push_internal(c);
        }
    }

    for (int pi = 0; pi < g_ptr_cnt; pi++) {
        int32_t dx = 0, dy = 0;
        bool lbtn = g_lbtn, rbtn = g_rbtn;
        bool had_event = false;
        int8_t scroll = 0;

        while (read(g_ptr_fds[pi], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            had_event = true;
            if (ev.type == EV_REL) {
                if (ev.code == REL_X)      dx += ev.value;
                else if (ev.code == REL_Y) dy += ev.value;
                else if (ev.code == REL_WHEEL) scroll += (int8_t)ev.value;
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT)  lbtn = (ev.value != 0);
                if (ev.code == BTN_RIGHT) rbtn = (ev.value != 0);
            }
        }

        if (had_event) {
            bool prev = g_lbtn;
            g_lbtn = lbtn; g_rbtn = rbtn;
            if (scroll) g_scroll_pending = scroll > 0 ? 1 : -1;
            mouse_push_rel(dx, dy, lbtn, rbtn);
            /* click on rising edge */
            if (lbtn && !prev) {
                if (g_clk_used < CLK_RING) {
                    g_clk_ring[(g_clk_head + g_clk_used) % CLK_RING] =
                        (click_t){ g_mx, g_my };
                    g_clk_used++;
                }
            }
        }
    }

    /* ── Absolute pointer devices (USB tablet / virtio-tablet) ───────────── */
    for (int ai = 0; ai < g_abs_cnt; ai++) {
        abs_dev_t *dev = &g_abs_devs[ai];
        int32_t abs_x = -1, abs_y = -1;
        bool lbtn = g_lbtn, rbtn = g_rbtn;
        bool had_event = false;

        while (read(dev->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) abs_x = ev.value;
                else if (ev.code == ABS_Y) abs_y = ev.value;
                had_event = true;
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT)  lbtn = (ev.value != 0);
                if (ev.code == BTN_RIGHT) rbtn = (ev.value != 0);
                had_event = true;
            } else if (ev.type == EV_SYN) {
                had_event = true;
            }
        }

        if (had_event) {
            bool prev = g_lbtn;
            g_lbtn = lbtn; g_rbtn = rbtn;
            /* Scale using actual device axis range */
            if (abs_x >= 0)
                g_mx = (int32_t)((int64_t)abs_x * g_fb_w / (dev->x_max + 1));
            if (abs_y >= 0)
                g_my = (int32_t)((int64_t)abs_y * g_fb_h / (dev->y_max + 1));
            if (g_mx < 0) g_mx = 0;
            if (g_my < 0) g_my = 0;
            if (g_mx >= g_fb_w) g_mx = g_fb_w - 1;
            if (g_my >= g_fb_h) g_my = g_fb_h - 1;
            if (lbtn && !prev) {
                if (g_clk_used < CLK_RING) {
                    g_clk_ring[(g_clk_head + g_clk_used) % CLK_RING] =
                        (click_t){ g_mx, g_my };
                    g_clk_used++;
                }
            }
        }
    }
}

/* ── keyboard.h API (implemented without including keyboard.h to avoid
 * KEY_* define conflicts with linux/input.h) ─────────────────────────────── */

void keyboard_push_char(uint8_t c) { kb_push_internal(c); }

int keyboard_try_getchar(void) {
    if (!g_kb_used) return -1;
    uint8_t c = g_kb_ring[g_kb_head];
    g_kb_head = (g_kb_head + 1) % KB_RING;
    g_kb_used--;
    return (int)(unsigned int)c;
}

void keyboard_set_gui_capture(bool on) {
    g_gui_capture = on;
    g_kb_used = g_gui_used = 0;
}

int keyboard_gui_try_getchar(void) {
    if (!g_gui_used) return -1;
    uint8_t c = g_gui_ring[g_gui_head];
    g_gui_head = (g_gui_head + 1) % GUI_RING;
    g_gui_used--;
    return (int)(unsigned int)c;
}

bool kbd_shift_down(void) { return g_shift; }
bool kbd_ctrl_down(void)  { return g_ctrl;  }
bool kbd_alt_down(void)   { return g_alt;   }

/* Stub keyboard functions unused on Linux */
void keyboard_on_scancode(uint8_t sc)     { (void)sc; }
uint64_t keyboard_irq_count(void)         { return 0; }
void keyboard_irq_handler(void)           { }
void keyboard_ps2_init(void)              { }
void keyboard_ps2_poll(void)              { }
void keyboard_repeat_tick(void)           { }
void keyboard_ps2_diag(void)              { }
void keyboard_ps2_full_init(void)         { }
uint32_t keyboard_sc_make(uint8_t sc)     { (void)sc; return 0; }
uint32_t keyboard_sc_break(uint8_t sc)    { (void)sc; return 0; }
void keyboard_clear_state(void)           { g_kb_used = g_gui_used = 0; }
void keyboard_hid_make(uint8_t kc, uint8_t ch) { (void)kc; keyboard_push_char(ch); }
void keyboard_hid_break(uint8_t kc)       { (void)kc; }
void keyboard_set_hid_present(void)       { }
void keyboard_set_raw_capture(int on)     { (void)on; }
uint32_t keyboard_raw_total(void)         { return 0; }
uint32_t keyboard_raw_aux(void)           { return 0; }
int keyboard_has_data(void)               { return g_kb_used > 0 ? 1 : 0; }

/* ── mouse.h API ─────────────────────────────────────────────────────────── */

static cursor_type_t g_cursor_type = CURSOR_ARROW;

void mouse_init(void) { g_mx = g_fb_w / 2; g_my = g_fb_h / 2; }

void mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn) {
    g_mx += dx; g_my += dy;
    if (g_mx < 0)       g_mx = 0;
    if (g_my < 0)       g_my = 0;
    if (g_mx >= g_fb_w) g_mx = g_fb_w - 1;
    if (g_my >= g_fb_h) g_my = g_fb_h - 1;
    g_lbtn = lbtn; g_rbtn = rbtn;
}

void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn) {
    if (x)    *x    = g_mx;
    if (y)    *y    = g_my;
    if (lbtn) *lbtn = g_lbtn;
    if (rbtn) *rbtn = g_rbtn;
}

bool mouse_consume_click(int32_t *x, int32_t *y) {
    if (!g_clk_used) return false;
    click_t c = g_clk_ring[g_clk_head];
    g_clk_head = (g_clk_head + 1) % CLK_RING;
    g_clk_used--;
    if (x) *x = c.x;
    if (y) *y = c.y;
    return true;
}

int8_t mouse_consume_scroll(void) {
    int8_t v = g_scroll_pending; g_scroll_pending = 0; return v;
}

void mouse_warp(int32_t x, int32_t y)   { g_mx = x; g_my = y; }
void mouse_click(int32_t x, int32_t y) {
    if (g_clk_used < CLK_RING)
        g_clk_ring[(g_clk_head + g_clk_used++) % CLK_RING] = (click_t){ x, y };
}
void mouse_irq_handler(void)            { }
void mouse_on_byte(uint8_t b)           { (void)b; }
void mouse_set_intellimouse(bool e)     { (void)e; }
void mouse_set_cursor(cursor_type_t t)  { g_cursor_type = t; }
cursor_type_t mouse_get_cursor(void)    { return g_cursor_type; }

/* ── Compositor-visible helpers ──────────────────────────────────────────── */

bool keyboard_gui_capture_active(void) { return g_gui_capture; }

int input_get_all_fds(int *buf, int maxn) {
    int n = 0;
    for (int i = 0; i < g_kbd_cnt && n < maxn; i++) buf[n++] = g_kbd_fds[i];
    for (int i = 0; i < g_ptr_cnt && n < maxn; i++) buf[n++] = g_ptr_fds[i];
    for (int i = 0; i < g_abs_cnt && n < maxn; i++) buf[n++] = g_abs_devs[i].fd;
    return n;
}
