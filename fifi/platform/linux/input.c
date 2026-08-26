#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <pthread.h>
#include <libinput.h>

/* Include mouse.h BEFORE linux/input.h — mouse.h has no KEY_* conflicts */
#include "mouse.h"
#include "console.h"
#include "touchpad_axis.h"
#include "touchpad_motion.h"
#include "../../shared/theme.h"

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
#define FIFI_KEY_SUPER_LEFT  0x98u
#define FIFI_KEY_SUPER_RIGHT 0x99u
#define FIFI_KEY_SUPER_UP    0x9Au
#define FIFI_KEY_SUPER_DOWN  0x9Bu
#define FIFI_KEY_SUPER_L     0x9Cu  /* Win+L = lock screen */
#define FIFI_KEY_SUPER_D     0x9Du  /* Win+D = show/hide desktop */
#define FIFI_KEY_SUPER       0x9Eu  /* bare Super tap (no combo) = toggle launcher */
#define FIFI_KEY_SUPER_HELP  0x9Fu  /* Super+/ = keyboard shortcuts overlay */
#define FIFI_KEY_PRTSC  0x96u  /* PrintScreen / SysRq */
#define FIFI_KEY_ALT_F4 0x97u  /* Alt+F4 — close focused window */

/* ── Evdev device fds ─────────────────────────────────────────────────────── */
#define MAX_EVDEV 32
static int g_kbd_fds[MAX_EVDEV];
static int g_kbd_cnt = 0;
static int g_ptr_fds[MAX_EVDEV];   /* relative mice */
static int g_ptr_cnt = 0;
typedef struct {
    int fd;
    int32_t x_min, x_max, y_min, y_max;
    int32_t x_fuzz, y_fuzz;
    bool is_mt;
    bool is_touchpad;  /* INPUT_PROP_POINTER: use delta tracking, not abs mapping */
    bool tool_doubletap;      /* two fingers on pad — BTN_LEFT while set = right-click */
    bool prev_tool_doubletap; /* previous frame state — detects 0→1 transition */
    bool btn_touch;      /* BTN_TOUCH state: finger physically on pad */
    int32_t click_cooldown;  /* frames to skip delta after BTN_LEFT change (spring-back) */
    int32_t lift_cooldown;   /* debounce ticks before motion anchor resets after lift */
    touchpad_motion_state_t motion;
    touchpad_axis_state_t axes; /* persistent coordinates and MT slot across polls */
    char phys[64];           /* EVIOCGPHYS — used to skip companion REL node */
} abs_dev_t;
static abs_dev_t g_abs_devs[MAX_EVDEV];
static int g_abs_cnt = 0;
static int g_hotplug_fd = -1;

#define MAX_LIBINPUT_DEVICES 64
typedef struct {
    struct libinput_device *device;
    char path[80];
    dev_t devno;
    ino_t inode;
    bool is_touchpad;
} libinput_path_t;
static struct libinput *g_libinput = NULL;
static libinput_path_t g_libinput_paths[MAX_LIBINPUT_DEVICES];
static int g_libinput_path_count = 0;
static int g_libinput_pointer_count = 0;
static double g_libinput_residual_x = 0.0;
static double g_libinput_residual_y = 0.0;
static int g_mouse_speed = FIFI_INPUT_DEFAULT_MOUSE_SPEED;
static int g_touchpad_speed = FIFI_INPUT_DEFAULT_TOUCHPAD_SPEED;

/* ── Gamepad state ──────────────────────────────────────────────────────────
 * Supports up to 2 gamepads. Each one tracks buttons + 4 axes + 2 triggers. */
#define GP_BTN_A      (1u<<0)
#define GP_BTN_B      (1u<<1)
#define GP_BTN_X      (1u<<2)
#define GP_BTN_Y      (1u<<3)
#define GP_BTN_LB     (1u<<4)
#define GP_BTN_RB     (1u<<5)
#define GP_BTN_START  (1u<<6)
#define GP_BTN_SELECT (1u<<7)
#define GP_BTN_LS     (1u<<8)
#define GP_BTN_RS     (1u<<9)
#define GP_BTN_DUP    (1u<<10)
#define GP_BTN_DDOWN  (1u<<11)
#define GP_BTN_DLEFT  (1u<<12)
#define GP_BTN_DRIGHT (1u<<13)

typedef struct {
    int      fd;
    uint16_t buttons;
    int16_t  lx, ly;   /* left stick */
    int16_t  rx, ry;   /* right stick */
    int16_t  lt, rt;   /* triggers */
    bool     changed;
    int32_t  abs_max[8]; /* max values for ABS 0..7 */
} gamepad_t;

static gamepad_t g_gamepads[2];
static int       g_gp_cnt = 0;

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
static bool g_caps  = false;
static bool g_ctrl  = false;
static bool g_alt   = false;
static bool g_super = false;
static bool g_super_used = false;  /* a combo key was pressed during the Super hold */

static void input_state_lock(void);
static void input_state_unlock(void);

/* Raw evdev key queue for Wayland clients (evdev code + press/release) */
#define RAW_KEY_RING 64
typedef struct { uint16_t code; uint8_t state; } raw_key_t;
static raw_key_t g_raw_ring[RAW_KEY_RING];
static int g_raw_head = 0, g_raw_tail = 0;
static void raw_key_push(uint16_t code, uint8_t state) {
    int next = (g_raw_tail + 1) % RAW_KEY_RING;
    if (next == g_raw_head) return;  /* full */
    g_raw_ring[g_raw_tail] = (raw_key_t){ code, state };
    g_raw_tail = next;
}
int keyboard_try_get_raw(uint16_t *code, uint8_t *state) {
    input_state_lock();
    if (g_raw_head == g_raw_tail) { input_state_unlock(); return 0; }
    *code  = g_raw_ring[g_raw_head].code;
    *state = g_raw_ring[g_raw_head].state;
    g_raw_head = (g_raw_head + 1) % RAW_KEY_RING;
    input_state_unlock();
    return 1;
}

/* ── Mouse state ─────────────────────────────────────────────────────────── */
static int32_t g_mx = 400, g_my = 300;
static bool    g_lbtn = false, g_rbtn = false;
static int32_t g_fb_w = 1024, g_fb_h = 768;

#define CLK_RING 16
typedef struct { int32_t x, y; } click_t;
static click_t  g_clk_ring[CLK_RING];
static uint32_t g_clk_head = 0;
static uint32_t g_clk_used = 0;
static click_t  g_rclk_ring[CLK_RING];
static uint32_t g_rclk_head = 0;
static uint32_t g_rclk_used = 0;
static click_t  g_deferred_clk[CLK_RING], g_deferred_rclk[CLK_RING];
static uint32_t g_deferred_clk_used = 0, g_deferred_rclk_used = 0;
static bool     g_motion_only = false;
static int8_t   g_scroll_pending = 0;

/* Input can be drained while a slow render owns the compositor lock. */
static pthread_mutex_t g_input_mx;
static pthread_once_t g_input_mx_once = PTHREAD_ONCE_INIT;
static void input_mx_init_once(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_input_mx, &attr);
    pthread_mutexattr_destroy(&attr);
}
static void input_state_lock(void) {
    pthread_once(&g_input_mx_once, input_mx_init_once);
    pthread_mutex_lock(&g_input_mx);
}
static void input_state_unlock(void) { pthread_mutex_unlock(&g_input_mx); }

static void input_queue_click(bool right, int32_t x, int32_t y) {
    click_t *ring;
    uint32_t *used;
    uint32_t head;
    if (g_motion_only) {
        ring = right ? g_deferred_rclk : g_deferred_clk;
        used = right ? &g_deferred_rclk_used : &g_deferred_clk_used;
        head = 0;
    } else {
        ring = right ? g_rclk_ring : g_clk_ring;
        used = right ? &g_rclk_used : &g_clk_used;
        head = right ? g_rclk_head : g_clk_head;
    }
    if (*used < CLK_RING) ring[(head + (*used)++) % CLK_RING] = (click_t){ x, y };
}

void input_flush_deferred_clicks(void) {
    input_state_lock();
    for (uint32_t i = 0; i < g_deferred_clk_used; i++)
        input_queue_click(false, g_deferred_clk[i].x, g_deferred_clk[i].y);
    for (uint32_t i = 0; i < g_deferred_rclk_used; i++)
        input_queue_click(true, g_deferred_rclk[i].x, g_deferred_rclk[i].y);
    g_deferred_clk_used = g_deferred_rclk_used = 0;
    input_state_unlock();
}

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
    /* Super+key combos */
    if (g_super) {
        switch (code) {
        case KEY_LEFT:  return FIFI_KEY_SUPER_LEFT;
        case KEY_RIGHT: return FIFI_KEY_SUPER_RIGHT;
        case KEY_UP:    return FIFI_KEY_SUPER_UP;
        case KEY_DOWN:  return FIFI_KEY_SUPER_DOWN;
        case KEY_L:     return FIFI_KEY_SUPER_L;
        case KEY_D:     return FIFI_KEY_SUPER_D;
        case KEY_SLASH: return FIFI_KEY_SUPER_HELP;
        default: break;
        }
    }
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
        case KEY_VOLUMEDOWN: return FIFI_KEY_F11;
        case KEY_VOLUMEUP:   return FIFI_KEY_F12;
        case KEY_SYSRQ: return FIFI_KEY_PRTSC;
        /* ASCII control */
        /* Backspace sends DEL (0x7f), not BS (0x08): the PTY line discipline's
         * erase char (VERASE) is 0x7f, and raw-mode TUIs (readline, Ink-based
         * apps) also expect 0x7f. Sending 0x08 left both unable to delete. */
        case KEY_BACKSPACE: return 0x7Fu;
        case KEY_TAB:       return '\t';
        case KEY_ENTER:     return '\n';
        case KEY_ESC:       return 0x1Bu;
        /* Numpad keys whose codes fall in the printable-key range */
        case KEY_KPASTERISK: return '*';
    }

    if (ctrl) {
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
            default: break;
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
    /* Numpad keys */
    switch (code) {
    case 71: return '7'; case 72: return '8'; case 73: return '9';
    case 75: return '4'; case 76: return '5'; case 77: return '6';
    case 79: return '1'; case 80: return '2'; case 81: return '3';
    case 82: return '0'; case 83: return '.';
    case 74: return '-'; case 78: return '+';
    case 98: return '/';
    case 96: return '\r';  /* KP_ENTER */
    default: break;
    }
    return 0;
}

/* System keys always reach the GUI ring regardless of capture mode. */
static inline bool is_system_key(uint8_t c) {
    return c >= FIFI_KEY_F1 && c <= FIFI_KEY_F12;
}

static void kb_push_internal(uint8_t c) {
    if (!c) return;
    if (g_gui_capture || is_system_key(c)) {
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
    input_state_lock();
    if (!g_fb_ptr) { input_state_unlock(); return; }

    uint32_t *bb       = console_backbuf_ptr();
    uint64_t  bb_pitch = console_backbuf_pitch32();
    if (!bb) bb_pitch = g_fb_pitch;

    /* ── Phase 1: restore old cursor position from backbuffer ─────────── */
    if (g_cur_saved_x >= 0) {
        int ox = g_cur_saved_x, oy = g_cur_saved_y;
        /* Fast path: cursor fully on-screen (no edge clamping needed) */
        if (ox >= 0 && oy >= 0 && ox + CUR_W <= g_fb_w && oy + CUR_H <= g_fb_h) {
            for (int cy = 0; cy < CUR_H; cy++) {
                uint32_t *dst = g_fb_ptr + (uint64_t)(oy + cy) * g_fb_pitch + ox;
                if (bb) {
                    const uint32_t *s = bb + (uint64_t)(oy + cy) * bb_pitch + ox;
                    for (int cx = 0; cx < CUR_W; cx++) dst[cx] = s[cx];
                } else {
                    for (int cx = 0; cx < CUR_W; cx++)
                        dst[cx] = g_cur_saved[cy * CUR_W + cx];
                }
            }
        } else {
            for (int cy = 0; cy < CUR_H; cy++) {
                int py = oy + cy;
                if (py < 0 || py >= g_fb_h) continue;
                for (int cx = 0; cx < CUR_W; cx++) {
                    int px = ox + cx;
                    if (px < 0 || px >= g_fb_w) continue;
                    g_fb_ptr[(uint64_t)py * g_fb_pitch + px] = bb
                        ? bb[(uint64_t)py * bb_pitch + px]
                        : g_cur_saved[cy * CUR_W + cx];
                }
            }
        }
    }

    /* ── Phase 2+3: save from backbuffer + draw cursor in a single pass ── */
    g_cur_saved_x = g_mx;
    g_cur_saved_y = g_my;
    const uint32_t *save_src = bb ? bb : g_fb_ptr;
    uint64_t        save_p   = bb ? bb_pitch : g_fb_pitch;

    int nx = g_mx, ny = g_my;
    if (nx >= 0 && ny >= 0 && nx + CUR_W <= g_fb_w && ny + CUR_H <= g_fb_h) {
        /* Fast path: cursor fully on-screen */
        for (int cy = 0; cy < CUR_H; cy++) {
            const uint32_t *ss  = save_src + (uint64_t)(ny + cy) * save_p + nx;
            uint32_t       *dst = g_fb_ptr + (uint64_t)(ny + cy) * g_fb_pitch + nx;
            const uint8_t  *row = s_cursor[cy];
            for (int cx = 0; cx < CUR_W; cx++) {
                g_cur_saved[cy * CUR_W + cx] = ss[cx];
                uint8_t v = row[cx];
                dst[cx] = v ? (v == 1 ? 0x00FFFFFFu : 0x00000000u) : ss[cx];
            }
        }
    } else {
        for (int cy = 0; cy < CUR_H; cy++) {
            int py = ny + cy;
            if (py < 0 || py >= g_fb_h) continue;
            for (int cx = 0; cx < CUR_W; cx++) {
                int px = nx + cx;
                if (px < 0 || px >= g_fb_w) continue;
                uint32_t bg = save_src[(uint64_t)py * save_p + px];
                g_cur_saved[cy * CUR_W + cx] = bg;
                uint8_t v = s_cursor[cy][cx];
                g_fb_ptr[(uint64_t)py * g_fb_pitch + px] =
                    v ? (v == 1 ? 0x00FFFFFFu : 0x00000000u) : bg;
            }
        }
    }
    input_state_unlock();
}

/* ── Evdev device detection ─────────────────────────────────────────────── */

static bool evdev_has_bit(int fd, int type, int bit) {
    uint8_t bits[96] = {0};
    ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits);
    return (bits[bit / 8] >> (bit % 8)) & 1;
}

static int libinput_open_restricted(const char *path, int flags,
                                    void *user_data) {
    (void)user_data;
    int fd = open(path, flags | O_CLOEXEC);
    return fd >= 0 ? fd : -errno;
}

static void libinput_close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface g_libinput_interface = {
    .open_restricted = libinput_open_restricted,
    .close_restricted = libinput_close_restricted,
};

static bool input_phys_has_raw_touchpad(const char *wanted_phys) {
    if (!wanted_phys || !wanted_phys[0]) return false;
    DIR *dir = opendir("/dev/input");
    if (!dir) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char phys[64] = "";
        uint8_t props[1] = {0};
        ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        ioctl(fd, EVIOCGPROP(sizeof(props)), props);
        phys[sizeof(phys) - 1] = '\0';
        bool pointer_prop = (props[0] >> INPUT_PROP_POINTER) & 1;
        bool abs_xy = evdev_has_bit(fd, 0, EV_ABS) &&
            ((evdev_has_bit(fd, EV_ABS, ABS_X) &&
              evdev_has_bit(fd, EV_ABS, ABS_Y)) ||
             (evdev_has_bit(fd, EV_ABS, ABS_MT_POSITION_X) &&
              evdev_has_bit(fd, EV_ABS, ABS_MT_POSITION_Y)));
        found = pointer_prop && abs_xy && strcmp(phys, wanted_phys) == 0;
        close(fd);
    }
    closedir(dir);
    return found;
}

static bool input_path_is_kernel_touchpad_companion(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    char phys[64] = "";
    ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
    phys[sizeof(phys) - 1] = '\0';
    bool relative_xy = evdev_has_bit(fd, 0, EV_REL) &&
                       evdev_has_bit(fd, EV_REL, REL_X) &&
                       evdev_has_bit(fd, EV_REL, REL_Y);
    close(fd);
    return relative_xy && input_phys_has_raw_touchpad(phys);
}

static bool input_libinput_path_known(const struct stat *st) {
    for (int i = 0; i < g_libinput_path_count; i++) {
        if (g_libinput_paths[i].devno == st->st_dev &&
                g_libinput_paths[i].inode == st->st_ino)
            return true;
    }
    return false;
}

static int input_speed_clamp(int speed) {
    if (speed < -100) return -100;
    if (speed > 100) return 100;
    return speed;
}

static void input_read_settings(void) {
    int mouse_speed = FIFI_INPUT_DEFAULT_MOUSE_SPEED;
    int touchpad_speed = FIFI_INPUT_DEFAULT_TOUCHPAD_SPEED;
    FILE *file = fopen(FIFI_THEME_CONFIG_PATH, "r");
    if (file) {
        char line[192];
        while (fgets(line, sizeof(line), file)) {
            int value;
            if (sscanf(line, FIFI_INPUT_KEY_MOUSE_SPEED "=%d", &value) == 1)
                mouse_speed = value;
            else if (sscanf(line, FIFI_INPUT_KEY_TOUCHPAD_SPEED "=%d", &value) == 1)
                touchpad_speed = value;
        }
        fclose(file);
    }
    g_mouse_speed = input_speed_clamp(mouse_speed);
    g_touchpad_speed = input_speed_clamp(touchpad_speed);
}

static void input_apply_device_settings(libinput_path_t *tracked) {
    if (!tracked || !tracked->device) return;
    int speed = tracked->is_touchpad ? g_touchpad_speed : g_mouse_speed;
    if (libinput_device_config_accel_is_available(tracked->device))
        libinput_device_config_accel_set_speed(tracked->device,
                                                (double)speed / 100.0);
}

void input_settings_reload(void) {
    input_state_lock();
    input_read_settings();
    for (int i = 0; i < g_libinput_path_count; i++)
        input_apply_device_settings(&g_libinput_paths[i]);
    fprintf(stderr, "[input] settings: mouse-speed=%d touchpad-speed=%d\n",
            g_mouse_speed, g_touchpad_speed);
    input_state_unlock();
}

static void input_libinput_add_path(const char *path) {
    if (!g_libinput || g_libinput_path_count >= MAX_LIBINPUT_DEVICES) return;
    struct stat st;
    if (stat(path, &st) < 0 || input_libinput_path_known(&st)) return;

    /* hid-multitouch exposes a synthetic REL companion beside the real ABS
     * touchpad. libinput wants the real touchpad; feeding both duplicates the
     * gesture. External USB mice have no matching raw touchpad and stay here. */
    if (input_path_is_kernel_touchpad_companion(path)) return;

    struct libinput_device *device = libinput_path_add_device(g_libinput, path);
    if (!device) return;
    if (!libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        libinput_path_remove_device(device);
        return;
    }

    int tap_fingers = libinput_device_config_tap_get_finger_count(device);
    const char *profile_name = tap_fingers > 0 ? "adaptive-touchpad" : "flat-mouse";
    if (libinput_device_config_accel_is_available(device)) {
        uint32_t profiles = libinput_device_config_accel_get_profiles(device);
        enum libinput_config_accel_profile requested = tap_fingers > 0
            ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
            : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
        if (profiles & (uint32_t)requested)
            libinput_device_config_accel_set_profile(device, requested);
    }

    libinput_path_t *tracked = &g_libinput_paths[g_libinput_path_count++];
    tracked->device = device;
    tracked->devno = st.st_dev;
    tracked->inode = st.st_ino;
    tracked->is_touchpad = tap_fingers > 0;
    snprintf(tracked->path, sizeof(tracked->path), "%s", path);
    input_apply_device_settings(tracked);
    g_libinput_pointer_count++;
    fprintf(stderr, "[input] libinput: %s \"%s\" profile=%s speed=%d\n", path,
            libinput_device_get_name(device), profile_name,
            tracked->is_touchpad ? g_touchpad_speed : g_mouse_speed);
}

static void input_libinput_rescan(void) {
    if (!g_libinput) return;
    for (int i = 0; i < g_libinput_path_count; ) {
        struct stat st;
        libinput_path_t *tracked = &g_libinput_paths[i];
        if (stat(tracked->path, &st) < 0 || st.st_dev != tracked->devno ||
                st.st_ino != tracked->inode) {
            libinput_path_remove_device(tracked->device);
            g_libinput_paths[i] = g_libinput_paths[--g_libinput_path_count];
            g_libinput_pointer_count--;
        } else {
            i++;
        }
    }
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", entry->d_name);
        input_libinput_add_path(path);
    }
    closedir(dir);
    libinput_dispatch(g_libinput);
}

static void input_libinput_shutdown(void) {
    if (!g_libinput) return;
    while (g_libinput_path_count > 0) {
        libinput_path_remove_device(
            g_libinput_paths[--g_libinput_path_count].device);
    }
    libinput_unref(g_libinput);
    g_libinput = NULL;
    g_libinput_pointer_count = 0;
}

static void input_record_backend(const char *backend) {
    FILE *file = fopen("/tmp/fifi-input-backend", "w");
    if (!file) return;
    fprintf(file, "%s\n", backend);
    fclose(file);
}

static void evdev_abs_axis_info(int fd, unsigned int code,
                                int32_t *minimum, int32_t *maximum,
                                int32_t *fuzz) {
    struct input_absinfo info;
    if (ioctl(fd, EVIOCGABS(code), &info) == 0 && info.maximum > info.minimum) {
        *minimum = info.minimum;
        *maximum = info.maximum;
        *fuzz = info.fuzz > 0 ? info.fuzz : 0;
    } else {
        *minimum = 0;
        *maximum = 32767;
        *fuzz = 0;
    }
}

static void input_remove_touchpad_abs_fallback(const char *phys) {
    if (!phys || !phys[0]) return;
    for (int i = 0; i < g_abs_cnt; ) {
        if (g_abs_devs[i].is_touchpad &&
                strcmp(g_abs_devs[i].phys, phys) == 0) {
            fprintf(stderr,
                    "[input] replaced raw touchpad fallback with kernel REL fd=%d\n",
                    g_abs_devs[i].fd);
            close(g_abs_devs[i].fd);
            g_abs_devs[i] = g_abs_devs[--g_abs_cnt];
        } else {
            i++;
        }
    }
}

/* Modern HID touchpads expose both raw absolute contacts and a kernel-created
 * relative mouse interface.  Prefer the latter: it already applies the
 * device-specific coordinate conversion and avoids rebuilding pointer motion
 * from noisy contact positions in the compositor. */
static bool input_has_rel_companion(const char *touchpad_phys) {
    if (!touchpad_phys || !touchpad_phys[0]) return false;
    DIR *dir = opendir("/dev/input");
    if (!dir) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char phys[64] = "";
        ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        phys[sizeof(phys) - 1] = '\0';
        found = strcmp(phys, touchpad_phys) == 0 &&
                evdev_has_bit(fd, 0, EV_REL) &&
                evdev_has_bit(fd, EV_REL, REL_X) &&
                evdev_has_bit(fd, EV_REL, REL_Y) &&
                evdev_has_bit(fd, EV_KEY, BTN_LEFT);
        close(fd);
    }
    closedir(dir);
    return found;
}

void input_init(void) {
    input_read_settings();
    g_libinput = libinput_path_create_context(&g_libinput_interface, NULL);
    if (!g_libinput)
        fprintf(stderr, "[input] libinput unavailable; using raw evdev fallback\n");

    g_hotplug_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_hotplug_fd >= 0 &&
        inotify_add_watch(g_hotplug_fd, "/dev/input",
                          IN_CREATE | IN_DELETE | IN_MOVED_TO |
                          IN_MOVED_FROM | IN_ATTRIB) < 0) {
        close(g_hotplug_fd);
        g_hotplug_fd = -1;
    }

    DIR *d = opendir("/dev/input");
    if (!d) {
        fprintf(stderr, "[input] /dev/input not found\n");
        input_libinput_shutdown();
        input_record_backend("unavailable");
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", de->d_name);
        input_libinput_add_path(path);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        name[sizeof(name) - 1] = '\0';   /* EVIOCGNAME may not NUL-terminate */
        char phys[64] = "";
        ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        phys[sizeof(phys) - 1] = '\0';

        bool has_key = evdev_has_bit(fd, 0, EV_KEY);
        bool has_rel = evdev_has_bit(fd, 0, EV_REL);
        bool has_abs = evdev_has_bit(fd, 0, EV_ABS);

        fprintf(stderr, "[input] %s \"%s\" key=%d rel=%d abs=%d\n",
                path, name, has_key, has_rel, has_abs);

        if (has_key && evdev_has_bit(fd, EV_KEY, KEY_A)) {
            if (g_kbd_cnt < MAX_EVDEV) {
                g_kbd_fds[g_kbd_cnt++] = fd;
                fprintf(stderr, "[input] -> keyboard\n");
                continue;
            }
        }

        /* Pointer: accept legacy ABS (ABS_X/Y) or MT Type B (ABS_MT_POSITION_X/Y) */
        bool has_legacy_abs = has_abs && evdev_has_bit(fd, EV_ABS, ABS_X) &&
                              evdev_has_bit(fd, EV_ABS, ABS_Y);
        bool has_mt_abs = has_abs && evdev_has_bit(fd, EV_ABS, ABS_MT_POSITION_X);
        bool has_touch = evdev_has_bit(fd, EV_KEY, BTN_LEFT) ||
                         evdev_has_bit(fd, EV_KEY, BTN_TOUCH) ||
                         evdev_has_bit(fd, EV_KEY, BTN_TOOL_FINGER);

        /* Distinguish touchscreen (INPUT_PROP_DIRECT) from touchpad (INPUT_PROP_POINTER).
         * Touchpads emit a companion Mouse node with REL_X/Y — skip the ABS node so
         * we use relative motion instead of absolute screen-coordinate mapping. */
        uint8_t prop_bits[1] = {0};
        ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits);
        bool is_direct  = (prop_bits[0] >> INPUT_PROP_DIRECT)  & 1;
        bool is_pointer = (prop_bits[0] >> INPUT_PROP_POINTER) & 1;

        /* Check absolute BEFORE relative — virtio-tablet has both EV_ABS and EV_REL */
        if ((has_legacy_abs || has_mt_abs) && has_touch) {
            if (is_pointer && !is_direct && input_has_rel_companion(phys)) {
                fprintf(stderr,
                        "[input] -> raw touchpad (skipped; using kernel REL companion)\n");
                close(fd);
                continue;
            }
            if (g_abs_cnt < MAX_EVDEV) {
                abs_dev_t *dev = &g_abs_devs[g_abs_cnt++];
                memset(dev, 0, sizeof(*dev));
                dev->fd   = fd;
                dev->is_mt = !has_legacy_abs && has_mt_abs;
                /* Prefer legacy axis range; fall back to MT axis range */
                if (has_legacy_abs) {
                    evdev_abs_axis_info(fd, ABS_X, &dev->x_min, &dev->x_max,
                                        &dev->x_fuzz);
                    evdev_abs_axis_info(fd, ABS_Y, &dev->y_min, &dev->y_max,
                                        &dev->y_fuzz);
                } else {
                    evdev_abs_axis_info(fd, ABS_MT_POSITION_X,
                                        &dev->x_min, &dev->x_max, &dev->x_fuzz);
                    evdev_abs_axis_info(fd, ABS_MT_POSITION_Y,
                                        &dev->y_min, &dev->y_max, &dev->y_fuzz);
                }
                /* INPUT_PROP_POINTER = touchpad: use delta tracking, not abs mapping.
                 * INPUT_PROP_DIRECT  = touchscreen/tablet: map abs coords to screen. */
                dev->is_touchpad = (is_pointer && !is_direct);
                touchpad_motion_reset(&dev->motion);
                touchpad_axis_reset(&dev->axes);
                snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
                fprintf(stderr,
                        "[input] -> %s %s axes x=%d..%d fuzz=%d y=%d..%d fuzz=%d\n",
                        dev->is_touchpad ? "touchpad" : "touchscreen/tablet",
                        dev->is_mt ? "(MT)" : "(abs)",
                        dev->x_min, dev->x_max, dev->x_fuzz,
                        dev->y_min, dev->y_max, dev->y_fuzz);
                continue;
            }
        }
        if (has_rel && evdev_has_bit(fd, EV_KEY, BTN_LEFT)) {
            /* If enumeration raced and installed the raw ABS fallback first,
             * replace it as soon as the preferred kernel REL node appears. */
            input_remove_touchpad_abs_fallback(phys);
            if (g_ptr_cnt < MAX_EVDEV) {
                g_ptr_fds[g_ptr_cnt++] = fd;
                fprintf(stderr, "[input] -> relative pointer\n");
                continue;
            }
        }
        /* Gamepad: has BTN_A (BTN_SOUTH=0x130) + EV_ABS */
        if (has_key && has_abs && evdev_has_bit(fd, EV_KEY, BTN_SOUTH) &&
            g_gp_cnt < 2) {
            gamepad_t *gp = &g_gamepads[g_gp_cnt++];
            memset(gp, 0, sizeof(*gp));
            gp->fd = fd;
            struct input_absinfo ai;
            for (int a = 0; a < 8; a++) {
                gp->abs_max[a] = (ioctl(fd, EVIOCGABS(a), &ai) == 0 && ai.maximum > 0)
                                 ? ai.maximum : 32767;
            }
            fprintf(stderr, "[input] gamepad: %s\n", path);
            continue;
        }
        close(fd);
    }
    closedir(d);

    if (g_libinput && g_libinput_pointer_count > 0) {
        libinput_dispatch(g_libinput);
        fprintf(stderr, "[input] libinput primary backend active (%d pointer nodes)\n",
                g_libinput_pointer_count);
        input_record_backend("libinput");
    } else if (g_libinput) {
        fprintf(stderr,
                "[input] libinput found no pointer devices; using raw evdev fallback\n");
        input_libinput_shutdown();
        input_record_backend("raw-evdev");
    } else {
        input_record_backend("raw-evdev");
    }

    if (g_kbd_cnt == 0) fprintf(stderr, "[input] warning: no keyboard found\n");
    if (g_ptr_cnt == 0 && g_abs_cnt == 0) fprintf(stderr, "[input] warning: no mouse found\n");
}

int input_hotplug_fd(void) {
    return g_hotplug_fd;
}

/* Drain queued directory changes. The caller rescans only when this reports a
 * real hotplug notification, keeping filesystem walks out of normal motion. */
bool input_hotplug_pending(void) {
    if (g_hotplug_fd < 0) return false;
    char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool changed = false;
    ssize_t got;
    while ((got = read(g_hotplug_fd, events, sizeof(events))) > 0)
        changed = true;
    return changed;
}

/* Drop devices whose fd has gone dead (unplugged / uinput destroyed).
 * Without this, repeated hotplug cycles leak list slots until input stops. */
static void input_prune_dead(void) {
    int v;
    for (int i = 0; i < g_kbd_cnt; ) {
        if (ioctl(g_kbd_fds[i], EVIOCGVERSION, &v) < 0) {
            fprintf(stderr, "[input] pruned dead keyboard fd=%d\n", g_kbd_fds[i]);
            close(g_kbd_fds[i]);
            g_kbd_fds[i] = g_kbd_fds[--g_kbd_cnt];
        } else i++;
    }
    for (int i = 0; i < g_ptr_cnt; ) {
        if (ioctl(g_ptr_fds[i], EVIOCGVERSION, &v) < 0) {
            fprintf(stderr, "[input] pruned dead mouse fd=%d\n", g_ptr_fds[i]);
            close(g_ptr_fds[i]);
            g_ptr_fds[i] = g_ptr_fds[--g_ptr_cnt];
        } else i++;
    }
    for (int i = 0; i < g_abs_cnt; ) {
        if (ioctl(g_abs_devs[i].fd, EVIOCGVERSION, &v) < 0) {
            fprintf(stderr, "[input] pruned dead abs fd=%d\n", g_abs_devs[i].fd);
            close(g_abs_devs[i].fd);
            g_abs_devs[i] = g_abs_devs[--g_abs_cnt];
        } else i++;
    }
}

/* Rescan /dev/input for newly plugged devices not already open.
 * Safe to call repeatedly; skips devices already in the fd lists. */
void input_rescan(void) {
    input_state_lock();
    input_libinput_rescan();
    input_prune_dead();
    DIR *d = opendir("/dev/input");
    if (!d) { input_state_unlock(); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[80];
        snprintf(path, sizeof(path), "/dev/input/%.60s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        /* Skip if already tracked — compare by inode (fd values differ on re-open) */
        bool known = false;
        struct stat sa, sb;
        if (fstat(fd, &sa) == 0) {
            for (int i = 0; i < g_kbd_cnt && !known; i++)
                if (fstat(g_kbd_fds[i], &sb) == 0 && sa.st_ino == sb.st_ino) known = true;
            for (int i = 0; i < g_ptr_cnt && !known; i++)
                if (fstat(g_ptr_fds[i], &sb) == 0 && sa.st_ino == sb.st_ino) known = true;
            for (int i = 0; i < g_abs_cnt && !known; i++)
                if (fstat(g_abs_devs[i].fd, &sb) == 0 && sa.st_ino == sb.st_ino) known = true;
        }
        if (known) { close(fd); continue; }
        /* New device — use the same pointer classification as startup.  A
         * formerly simplified rescan added an intentionally-skipped touchpad
         * companion back as a mouse on its first inotify notification. */
        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        name[sizeof(name) - 1] = '\0';
        char phys[64] = "";
        ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        phys[sizeof(phys) - 1] = '\0';
        bool has_key = evdev_has_bit(fd, 0, EV_KEY);
        bool has_rel = evdev_has_bit(fd, 0, EV_REL);
        bool has_abs = evdev_has_bit(fd, 0, EV_ABS);
        if (has_key && evdev_has_bit(fd, EV_KEY, KEY_A) && g_kbd_cnt < MAX_EVDEV) {
            g_kbd_fds[g_kbd_cnt++] = fd;
            fprintf(stderr, "[input] rescan: new keyboard %s\n", path);
            continue;
        }
        bool has_legacy_abs = has_abs && evdev_has_bit(fd, EV_ABS, ABS_X) && evdev_has_bit(fd, EV_ABS, ABS_Y);
        bool has_mt_abs = has_abs && evdev_has_bit(fd, EV_ABS, ABS_MT_POSITION_X) &&
                          evdev_has_bit(fd, EV_ABS, ABS_MT_POSITION_Y);
        bool has_rel_xy = has_rel && evdev_has_bit(fd, EV_REL, REL_X) && evdev_has_bit(fd, EV_REL, REL_Y);
        bool has_touch = evdev_has_bit(fd, EV_KEY, BTN_LEFT) ||
                         evdev_has_bit(fd, EV_KEY, BTN_TOUCH) ||
                         evdev_has_bit(fd, EV_KEY, BTN_TOOL_FINGER);
        uint8_t prop_bits[1] = {0};
        ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits);
        bool is_direct  = (prop_bits[0] >> INPUT_PROP_DIRECT) & 1;
        bool is_pointer = (prop_bits[0] >> INPUT_PROP_POINTER) & 1;

        if ((has_legacy_abs || has_mt_abs) && has_touch &&
                g_abs_cnt < MAX_EVDEV) {
            if (is_pointer && !is_direct && input_has_rel_companion(phys)) {
                fprintf(stderr,
                        "[input] rescan: raw touchpad still skipped %s\n", path);
                close(fd);
                continue;
            }
            abs_dev_t *dev = &g_abs_devs[g_abs_cnt++];
            memset(dev, 0, sizeof(*dev));
            dev->fd = fd;
            dev->is_mt = !has_legacy_abs && has_mt_abs;
            dev->is_touchpad = is_pointer && !is_direct;
            unsigned int x_code = has_legacy_abs ? ABS_X : ABS_MT_POSITION_X;
            unsigned int y_code = has_legacy_abs ? ABS_Y : ABS_MT_POSITION_Y;
            evdev_abs_axis_info(fd, x_code, &dev->x_min, &dev->x_max,
                                &dev->x_fuzz);
            evdev_abs_axis_info(fd, y_code, &dev->y_min, &dev->y_max,
                                &dev->y_fuzz);
            touchpad_motion_reset(&dev->motion);
            touchpad_axis_reset(&dev->axes);
            snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
            fprintf(stderr, "[input] rescan: new %s %s \"%s\"\n",
                    dev->is_touchpad ? "touchpad" : "absolute pointer",
                    path, name);
            continue;
        }
        if (has_rel_xy && evdev_has_bit(fd, EV_KEY, BTN_LEFT)) {
            input_remove_touchpad_abs_fallback(phys);
            if (g_ptr_cnt < MAX_EVDEV) {
                g_ptr_fds[g_ptr_cnt++] = fd;
                fprintf(stderr,
                        "[input] rescan: new relative pointer %s \"%s\"\n",
                        path, name);
                continue;
            }
        }
        close(fd);
    }
    closedir(d);
    input_state_unlock();
}

/* ── Poll — call each frame ─────────────────────────────────────────────── */

static void input_poll_libinput(void) {
    if (!g_libinput || libinput_dispatch(g_libinput) < 0) return;
    struct libinput_event *event;
    while ((event = libinput_get_event(g_libinput)) != NULL) {
        enum libinput_event_type type = libinput_event_get_type(event);
        struct libinput_event_pointer *pointer =
            libinput_event_get_pointer_event(event);
        if (!pointer) {
            libinput_event_destroy(event);
            continue;
        }

        if (type == LIBINPUT_EVENT_POINTER_MOTION) {
            g_libinput_residual_x += libinput_event_pointer_get_dx(pointer);
            g_libinput_residual_y += libinput_event_pointer_get_dy(pointer);
            int32_t dx = (int32_t)g_libinput_residual_x;
            int32_t dy = (int32_t)g_libinput_residual_y;
            g_libinput_residual_x -= dx;
            g_libinput_residual_y -= dy;
            if (dx || dy) mouse_push_rel(dx, dy, g_lbtn, g_rbtn);
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE) {
            g_mx = (int32_t)libinput_event_pointer_get_absolute_x_transformed(
                pointer, (uint32_t)g_fb_w);
            g_my = (int32_t)libinput_event_pointer_get_absolute_y_transformed(
                pointer, (uint32_t)g_fb_h);
            if (g_mx < 0) g_mx = 0;
            if (g_my < 0) g_my = 0;
            if (g_mx >= g_fb_w) g_mx = g_fb_w - 1;
            if (g_my >= g_fb_h) g_my = g_fb_h - 1;
        } else if (type == LIBINPUT_EVENT_POINTER_BUTTON) {
            uint32_t button = libinput_event_pointer_get_button(pointer);
            bool pressed = libinput_event_pointer_get_button_state(pointer) ==
                           LIBINPUT_BUTTON_STATE_PRESSED;
            if (button == BTN_LEFT) {
                bool previous = g_lbtn;
                g_lbtn = pressed;
                mouse_push_rel(0, 0, g_lbtn, g_rbtn);
                if (pressed && !previous)
                    input_queue_click(false, g_mx, g_my);
            } else if (button == BTN_RIGHT) {
                bool previous = g_rbtn;
                g_rbtn = pressed;
                mouse_push_rel(0, 0, g_lbtn, g_rbtn);
                if (pressed && !previous)
                    input_queue_click(true, g_mx, g_my);
            }
        } else if (type == LIBINPUT_EVENT_POINTER_AXIS ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS) {
            if (libinput_event_pointer_has_axis(
                    pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
                double value;
                if (type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)
                    value = libinput_event_pointer_get_scroll_value_v120(
                        pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                else
                    value = libinput_event_pointer_get_scroll_value(
                        pointer, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                /* libinput: positive is down; FiFi's existing wheel contract
                 * uses positive for up. */
                if (value > 0.0) g_scroll_pending = -1;
                else if (value < 0.0) g_scroll_pending = 1;
            }
        }
        libinput_event_destroy(event);
    }
}

static void input_poll_mode(bool poll_pointer, bool poll_controls) {
    input_state_lock();
    g_motion_only = !poll_controls;
    struct input_event ev;

    if (poll_pointer && g_libinput)
        input_poll_libinput();

    for (int ki = 0; poll_controls && ki < g_kbd_cnt; ki++) {
        while (read(g_kbd_fds[ki], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type != EV_KEY) continue;
            bool pressed = (ev.value == 1 || ev.value == 2);
            /* Always enqueue raw evdev event for Wayland key forwarding */
            raw_key_push((uint16_t)ev.code, ev.value ? 1 : 0);

            if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
                g_shift = pressed;
            else if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
                g_ctrl = pressed;
            else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
                g_alt = pressed;
            else if (ev.code == KEY_LEFTMETA || ev.code == KEY_RIGHTMETA) {
                /* a bare tap (press+release, no other key in between) toggles
                 * the launcher — combos (Super+arrow etc.) suppress it */
                if (pressed && !g_super) g_super_used = false;
                if (!pressed && g_super && !g_super_used)
                    kb_push_internal(FIFI_KEY_SUPER);
                g_super = pressed;
            }
            else if (ev.code == KEY_CAPSLOCK && pressed)
                g_caps = !g_caps;   /* toggle on each key-down */
            else if (pressed && g_super)
                g_super_used = true;

            if (!pressed) continue;

            if (g_alt && ev.code == KEY_TAB) {
                kb_push_internal(FIFI_KEY_ALTTAB);
                continue;
            }
            if (g_alt && ev.code == KEY_F4) {
                kb_push_internal(FIFI_KEY_ALT_F4);
                continue;
            }

            bool effective_shift = g_shift;
            if (g_caps && ((ev.code >= 16 && ev.code <= 25) ||
                           (ev.code >= 30 && ev.code <= 38) ||
                           (ev.code >= 44 && ev.code <= 50)))
                effective_shift = !g_shift;
            uint8_t c = evkey_to_fifi((uint16_t)ev.code, effective_shift, g_ctrl);
            if (c) kb_push_internal(c);
        }
    }

    for (int pi = 0; poll_pointer && !g_libinput && pi < g_ptr_cnt; pi++) {
        int32_t dx = 0, dy = 0;
        bool lbtn = g_lbtn, rbtn = g_rbtn;
        bool had_event = false;
        bool dropped_report = false;
        int8_t scroll = 0;

        while (read(g_ptr_fds[pi], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_SYN) {
                if (ev.code == SYN_DROPPED) {
                    /* The kernel overran its client queue.  Discard the partial
                     * report instead of turning it into a large pointer jump. */
                    dx = dy = 0;
                    scroll = 0;
                    had_event = false;
                    dropped_report = true;
                    continue;
                }
                if (ev.code == SYN_REPORT) break;
            }
            if (dropped_report) continue;
            had_event = true;
            if (ev.type == EV_REL) {
                if (ev.code == REL_X)      dx += ev.value;
                else if (ev.code == REL_Y) dy += ev.value;
                else if (ev.code == REL_WHEEL) scroll += (int8_t)ev.value;
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT)  lbtn = (ev.value != 0);
                if (ev.code == BTN_RIGHT) {
                    bool new_r = (ev.value != 0);
                    if (new_r && !rbtn) {  /* rising edge — capture before release can clear it */
                        input_queue_click(true, g_mx, g_my);
                    }
                    rbtn = new_r;
                }
            }
        }

        if (had_event) {
            bool prev_l = g_lbtn;
            g_lbtn = lbtn; g_rbtn = rbtn;
            if (scroll) g_scroll_pending = scroll > 0 ? 1 : -1;
            mouse_push_rel(dx, dy, lbtn, rbtn);
            if (lbtn && !prev_l) {
                input_queue_click(false, g_mx, g_my);
            }
        }
    }

    /* ── Absolute pointer devices (USB tablet / virtio-tablet / touchpad) ── */
    for (int ai = 0; poll_pointer && !g_libinput && ai < g_abs_cnt; ai++) {
        abs_dev_t *dev = &g_abs_devs[ai];
        int32_t abs_x = dev->axes.x[0], abs_y = dev->axes.y[0];
        int32_t abs_x1 = dev->axes.x[1], abs_y1 = dev->axes.y[1];
        bool lbtn = g_lbtn, rbtn = g_rbtn;
        bool had_event = false;
        bool dropped_report = false;
        int8_t scroll = 0;  /* wheel: virtio-tablet reports EV_ABS + EV_REL on one node */
        int cur_slot = dev->axes.current_slot;

        /* Debounce: count down after finger lift; reset origin when it expires */
        if (dev->lift_cooldown > 0 && --dev->lift_cooldown == 0) {
            touchpad_motion_reset(&dev->motion);
        }

        while (read(dev->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_SYN) {
                if (ev.code == SYN_DROPPED) {
                    /* Do not combine coordinates from opposite sides of an evdev
                     * queue overrun.  The first complete report after this one
                     * establishes a fresh touchpad anchor. */
                    dropped_report = true;
                    had_event = false;
                    scroll = 0;
                    abs_x = abs_y = abs_x1 = abs_y1 = -1;
                    touchpad_axis_reset(&dev->axes);
                    touchpad_motion_reset(&dev->motion);
                    continue;
                }
                if (ev.code == SYN_REPORT) break;
            }
            if (dropped_report) continue;
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                /* ABS_X is the legacy single-touch axis.  When 2 fingers are on the
                 * pad (tool_doubletap) the kernel synthesises ABS_X by alternating
                 * between slot 0 and slot 1 positions, causing wild cursor jumps.
                 * Ignore it in that state; ABS_MT_POSITION_X slot 0/1 are used instead. */
                case ABS_X:
                    if (!dev->tool_doubletap) { abs_x = ev.value; dev->axes.x[0] = ev.value; }
                    break;
                case ABS_Y:
                    if (!dev->tool_doubletap) { abs_y = ev.value; dev->axes.y[0] = ev.value; }
                    break;
                case ABS_MT_SLOT:
                    cur_slot = ev.value;
                    touchpad_axis_select_slot(&dev->axes, cur_slot);
                    break;
                /* MT Type B: track slot 0 (click/anchor finger) and slot 1 (drag finger) */
                case ABS_MT_POSITION_X:
                    touchpad_axis_set_x(&dev->axes, ev.value);
                    if (cur_slot == 0) abs_x = ev.value;
                    else if (cur_slot == 1) abs_x1 = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    touchpad_axis_set_y(&dev->axes, ev.value);
                    if (cur_slot == 0) abs_y = ev.value;
                    else if (cur_slot == 1) abs_y1 = ev.value;
                    break;
                /* MT tracking ID = -1 means finger lifted */
                case ABS_MT_TRACKING_ID:
                    if (cur_slot == 0) {
                        if (ev.value == -1) {
                            /* Finger lifted — check BTN_TOUCH first.
                             * If BTN_TOUCH=1 the finger is still physically on
                             * the pad; id cycling is a light-touch artefact.
                             * Keep abs_x (ABS_X legacy axis stays current) so
                             * deltas are computed without stalling.
                             * If BTN_TOUCH=0 (or not yet received) this is a
                             * real lift — debounce before resetting origin. */
                            if (!dev->btn_touch) {
                                if (dev->lift_cooldown == 0)
                                    dev->lift_cooldown = 100;
                                abs_x = -1;
                                abs_y = -1;
                            }
                        } else {
                            /* New contact — cancel pending debounce reset.
                             * If btn_touch=0 (real lift already seen), the finger
                             * may land at a different raw position. Reset the anchor
                             * so the first abs_x is an anchor, not a delta source.
                             * This prevents new-contact cursor jumps.
                             * If btn_touch=1 (light-touch id cycling, finger never
                             * left), keep the anchor so movement stays continuous. */
                            dev->lift_cooldown = 0;
                            if (!dev->btn_touch) {
                                touchpad_motion_reset(&dev->motion);
                            }
                        }
                    }
                    break;
                default: break;
                }
                had_event = true;
            } else if (ev.type == EV_KEY) {
                if (dev->is_touchpad) {
                    /* Track two-finger tool state for buttonpad right-click emulation.
                     * Many clickpads (e.g. FTCS0038) have no BTN_RIGHT — two-finger
                     * physical click (BTN_LEFT while BTN_TOOL_DOUBLETAP is active)
                     * is how right-click is expressed. */
                    if (ev.code == BTN_TOOL_DOUBLETAP)
                        dev->tool_doubletap = (ev.value != 0);

                    if (ev.code == BTN_TOUCH) {
                        bool was = dev->btn_touch;
                        dev->btn_touch = (ev.value != 0);
                        if (!dev->btn_touch) {
                            /* Finger lifted — start debounce before resetting origin. */
                            if (dev->lift_cooldown == 0)
                                dev->lift_cooldown = 100;
                            touchpad_axis_reset(&dev->axes);
                            abs_x = abs_y = abs_x1 = abs_y1 = -1;
                        } else {
                            dev->lift_cooldown = 0;
                            /* Fresh contact from lifted state: reset anchor so the
                             * first abs_x of the new contact doesn't produce a delta
                             * against the old position (which could be far away). */
                            if (!was) {
                                touchpad_motion_reset(&dev->motion);
                            }
                        }
                    }

                    /* BTN_TOUCH = finger contact, not a click — ignore for lbtn.
                     * Only BTN_LEFT (physical clickpad press) counts.
                     * Reset prev on click AND release: the clickpad mechanism
                     * physically shifts the pad on press and springs back on
                     * release, both causing position jumps. Resetting absorbs them. */
                    if (ev.code == BTN_LEFT) {
                        bool new_lbtn = (ev.value != 0);
                        bool changed  = (new_lbtn != lbtn);
                        /* Always absorb spring-back regardless of click type */
                        if (changed) {
                            touchpad_motion_reset(&dev->motion);
                            dev->click_cooldown = 4;
                        }
                        if (dev->tool_doubletap) {
                            /* Two-finger click = right-click on buttonpad */
                            bool new_r = new_lbtn;
                            if (new_r && !rbtn) {
                                input_queue_click(true, g_mx, g_my);
                            }
                            rbtn = new_r;
                            /* Do NOT update lbtn — prevent a left-click from also firing */
                        } else {
                            lbtn = new_lbtn;
                        }
                    }
                    if (ev.code == BTN_RIGHT) {
                        bool new_r = (ev.value != 0);
                        if (new_r && !rbtn) {
                            input_queue_click(true, g_mx, g_my);
                        }
                        rbtn = new_r;
                    }
                } else {
                    if (ev.code == BTN_LEFT || ev.code == BTN_TOUCH)
                        lbtn = (ev.value != 0);
                    if (ev.code == BTN_RIGHT) {
                        bool new_r = (ev.value != 0);
                        if (new_r && !rbtn) {
                            input_queue_click(true, g_mx, g_my);
                        }
                        rbtn = new_r;
                    }
                }
                had_event = true;
            } else if (ev.type == EV_REL) {
                /* The virtio-tablet (and some touchscreens/tablets) carry the scroll
                 * wheel as REL_WHEEL on the same node as their absolute axes. */
                if (ev.code == REL_WHEEL) scroll += (int8_t)ev.value;
                had_event = true;
            }
        }

        if (had_event) {
            bool prev = g_lbtn;
            g_lbtn = lbtn; g_rbtn = rbtn;
            if (scroll) g_scroll_pending = scroll > 0 ? 1 : -1;
            if (dev->is_touchpad) {
                /* 2-finger mode (doubletap): slot 0 = click/anchor finger, slot 1 = drag finger.
                 * On transition 0→1 reset EMA so the new finger position is an anchor, not a delta.
                 * When doubletap active and slot 1 reported a position, use it as movement source. */
                bool cur_dt = dev->tool_doubletap;
                if (cur_dt && !dev->prev_tool_doubletap) {
                    touchpad_motion_reset(&dev->motion);
                }
                dev->prev_tool_doubletap = cur_dt;
                if (cur_dt && abs_x1 >= 0 && abs_y1 >= 0) {
                    abs_x = abs_x1;
                    abs_y = abs_y1;
                }

                /* Translate absolute pad coordinates into immediate relative motion.
                 * The shared filter preserves subpixel movement, uses independent
                 * X/Y ranges, and accelerates quick swipes without trailing the
                 * finger through a multi-poll position average. */
                if (abs_x >= 0 && abs_y >= 0) {
                    if (dev->click_cooldown > 0) {
                        /* Skip delta for a few frames after BTN_LEFT change to
                         * absorb the clickpad spring-back position drift. */
                        dev->click_cooldown--;
                        touchpad_motion_reset(&dev->motion);
                        int32_t ignored_x, ignored_y;
                        touchpad_motion_update(&dev->motion, abs_x, abs_y,
                                               dev->x_max - dev->x_min + 1,
                                               dev->y_max - dev->y_min + 1,
                                               dev->x_fuzz, dev->y_fuzz,
                                               g_fb_w, g_fb_h,
                                               &ignored_x, &ignored_y);
                    } else {
                        int32_t dxpx, dypx;
                        if (touchpad_motion_update(&dev->motion, abs_x, abs_y,
                                                   dev->x_max - dev->x_min + 1,
                                                   dev->y_max - dev->y_min + 1,
                                                   dev->x_fuzz, dev->y_fuzz,
                                                   g_fb_w, g_fb_h,
                                                   &dxpx, &dypx))
                            mouse_push_rel(dxpx, dypx, lbtn, rbtn);
                    }
                }
            } else {
                /* Touchscreen / tablet: map abs coords directly to screen */
                if (abs_x >= 0)
                    g_mx = (int32_t)((int64_t)(abs_x - dev->x_min) * g_fb_w /
                                     (dev->x_max - dev->x_min + 1));
                if (abs_y >= 0)
                    g_my = (int32_t)((int64_t)(abs_y - dev->y_min) * g_fb_h /
                                     (dev->y_max - dev->y_min + 1));
                if (g_mx < 0) g_mx = 0;
                if (g_my < 0) g_my = 0;
                if (g_mx >= g_fb_w) g_mx = g_fb_w - 1;
                if (g_my >= g_fb_h) g_my = g_fb_h - 1;
            }
            if (lbtn && !prev) {
                input_queue_click(false, g_mx, g_my);
            }
        }
    }

    /* ── Gamepad events ────────────────────────────────────────────────── */
    for (int gi = 0; poll_controls && gi < g_gp_cnt; gi++) {
        gamepad_t *gp = &g_gamepads[gi];
        struct input_event ev;
        gp->changed = false;
        while (read(gp->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                uint16_t bit = 0;
                switch (ev.code) {
                case BTN_SOUTH:   bit = GP_BTN_A;      break;
                case BTN_EAST:    bit = GP_BTN_B;      break;
                case BTN_NORTH:   bit = GP_BTN_X;      break;
                case BTN_WEST:    bit = GP_BTN_Y;      break;
                case BTN_TL:      bit = GP_BTN_LB;     break;
                case BTN_TR:      bit = GP_BTN_RB;     break;
                case BTN_START:   bit = GP_BTN_START;  break;
                case BTN_SELECT:  bit = GP_BTN_SELECT; break;
                case BTN_THUMBL:  bit = GP_BTN_LS;     break;
                case BTN_THUMBR:  bit = GP_BTN_RS;     break;
                }
                if (bit) {
                    if (ev.value) gp->buttons |=  bit;
                    else          gp->buttons &= ~bit;
                    gp->changed = true;
                }
            } else if (ev.type == EV_ABS) {
                int32_t max = (ev.code < 8) ? gp->abs_max[ev.code] : 32767;
                if (max == 0) max = 32767;
                /* Normalize to -32767..32767 (triggers: 0..32767) */
                int32_t n = (int32_t)((int64_t)ev.value * 32767 / max);
                switch (ev.code) {
                case ABS_X:     gp->lx = (int16_t)n; gp->changed = true; break;
                case ABS_Y:     gp->ly = (int16_t)n; gp->changed = true; break;
                case ABS_Z:     gp->lt = (int16_t)n; gp->changed = true; break;
                case ABS_RX:    gp->rx = (int16_t)n; gp->changed = true; break;
                case ABS_RY:    gp->ry = (int16_t)n; gp->changed = true; break;
                case ABS_RZ:    gp->rt = (int16_t)n; gp->changed = true; break;
                case ABS_HAT0X:
                    gp->buttons &= ~(GP_BTN_DLEFT | GP_BTN_DRIGHT);
                    if (ev.value < 0) gp->buttons |= GP_BTN_DLEFT;
                    if (ev.value > 0) gp->buttons |= GP_BTN_DRIGHT;
                    gp->changed = true; break;
                case ABS_HAT0Y:
                    gp->buttons &= ~(GP_BTN_DUP | GP_BTN_DDOWN);
                    if (ev.value < 0) gp->buttons |= GP_BTN_DUP;
                    if (ev.value > 0) gp->buttons |= GP_BTN_DDOWN;
                    gp->changed = true; break;
                }
            }
        }
    }
    g_motion_only = false;
    input_state_unlock();
}

void input_poll(void) { input_poll_mode(true, true); }
void input_poll_motion(void) { input_poll_mode(true, false); }
void input_poll_controls(void) { input_poll_mode(false, true); }

/* Gamepad query API */
bool input_gamepad_connected(void)   {
    input_state_lock(); bool connected = g_gp_cnt > 0; input_state_unlock(); return connected;
}

bool input_gamepad_state(int idx, uint16_t *btns,
                         int16_t *lx, int16_t *ly,
                         int16_t *rx, int16_t *ry,
                         int16_t *lt, int16_t *rt) {
    input_state_lock();
    if (idx < 0 || idx >= g_gp_cnt) { input_state_unlock(); return false; }
    gamepad_t *gp = &g_gamepads[idx];
    if (btns) *btns = gp->buttons;
    if (lx)   *lx   = gp->lx;
    if (ly)   *ly   = gp->ly;
    if (rx)   *rx   = gp->rx;
    if (ry)   *ry   = gp->ry;
    if (lt)   *lt   = gp->lt;
    if (rt)   *rt   = gp->rt;
    input_state_unlock();
    return true;
}

bool input_gamepad_changed(int idx) {
    input_state_lock();
    if (idx < 0 || idx >= g_gp_cnt) { input_state_unlock(); return false; }
    bool c = g_gamepads[idx].changed;
    g_gamepads[idx].changed = false;
    input_state_unlock();
    return c;
}

/* ── keyboard.h API (implemented without including keyboard.h to avoid
 * KEY_* define conflicts with linux/input.h) ─────────────────────────────── */

void keyboard_push_char(uint8_t c) { input_state_lock(); kb_push_internal(c); input_state_unlock(); }

int keyboard_try_getchar(void) {
    input_state_lock();
    if (!g_kb_used) { input_state_unlock(); return -1; }
    uint8_t c = g_kb_ring[g_kb_head];
    g_kb_head = (g_kb_head + 1) % KB_RING;
    g_kb_used--;
    input_state_unlock();
    return (int)(unsigned int)c;
}

void keyboard_set_gui_capture(bool on) {
    input_state_lock();
    g_gui_capture = on;
    g_kb_used = g_gui_used = 0;
    input_state_unlock();
}

int keyboard_gui_try_getchar(void) {
    input_state_lock();
    if (!g_gui_used) { input_state_unlock(); return -1; }
    uint8_t c = g_gui_ring[g_gui_head];
    g_gui_head = (g_gui_head + 1) % GUI_RING;
    g_gui_used--;
    input_state_unlock();
    return (int)(unsigned int)c;
}

bool kbd_shift_down(void) { input_state_lock(); bool v = g_shift; input_state_unlock(); return v; }
bool kbd_ctrl_down(void)  { input_state_lock(); bool v = g_ctrl;  input_state_unlock(); return v; }
bool kbd_alt_down(void)   { input_state_lock(); bool v = g_alt;   input_state_unlock(); return v; }

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
void keyboard_clear_state(void)           { input_state_lock(); g_kb_used = g_gui_used = 0; input_state_unlock(); }
void keyboard_hid_make(uint8_t kc, uint8_t ch) { (void)kc; keyboard_push_char(ch); }
void keyboard_hid_break(uint8_t kc)       { (void)kc; }
void keyboard_set_hid_present(void)       { }
void keyboard_set_raw_capture(int on)     { (void)on; }
uint32_t keyboard_raw_total(void)         { return 0; }
uint32_t keyboard_raw_aux(void)           { return 0; }
int keyboard_has_data(void)               { input_state_lock(); int v = g_kb_used > 0 ? 1 : 0; input_state_unlock(); return v; }

/* ── mouse.h API ─────────────────────────────────────────────────────────── */

static cursor_type_t g_cursor_type = CURSOR_ARROW;

void mouse_init(void) { g_mx = g_fb_w / 2; g_my = g_fb_h / 2; }

void mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn) {
    input_state_lock();
    g_mx += dx; g_my += dy;
    if (g_mx < 0)       g_mx = 0;
    if (g_my < 0)       g_my = 0;
    if (g_mx >= g_fb_w) g_mx = g_fb_w - 1;
    if (g_my >= g_fb_h) g_my = g_fb_h - 1;
    g_lbtn = lbtn; g_rbtn = rbtn;
    input_state_unlock();
}

void mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn) {
    input_state_lock();
    if (x)    *x    = g_mx;
    if (y)    *y    = g_my;
    if (lbtn) *lbtn = g_lbtn;
    if (rbtn) *rbtn = g_rbtn;
    input_state_unlock();
}

bool mouse_consume_click(int32_t *x, int32_t *y) {
    input_state_lock();
    if (!g_clk_used) { input_state_unlock(); return false; }
    click_t c = g_clk_ring[g_clk_head];
    g_clk_head = (g_clk_head + 1) % CLK_RING;
    g_clk_used--;
    if (x) *x = c.x;
    if (y) *y = c.y;
    input_state_unlock();
    return true;
}

bool mouse_consume_rclick(int32_t *x, int32_t *y) {
    input_state_lock();
    if (!g_rclk_used) { input_state_unlock(); return false; }
    click_t c = g_rclk_ring[g_rclk_head];
    g_rclk_head = (g_rclk_head + 1) % CLK_RING;
    g_rclk_used--;
    if (x) *x = c.x;
    if (y) *y = c.y;
    input_state_unlock();
    return true;
}

int8_t mouse_consume_scroll(void) {
    input_state_lock();
    int8_t v = g_scroll_pending; g_scroll_pending = 0;
    input_state_unlock();
    return v;
}

void mouse_warp(int32_t x, int32_t y)   { input_state_lock(); g_mx = x; g_my = y; input_state_unlock(); }
void mouse_click(int32_t x, int32_t y) {
    input_state_lock();
    if (g_clk_used < CLK_RING)
        g_clk_ring[(g_clk_head + g_clk_used++) % CLK_RING] = (click_t){ x, y };
    input_state_unlock();
}
void mouse_irq_handler(void)            { }
void mouse_on_byte(uint8_t b)           { (void)b; }
void mouse_set_intellimouse(bool e)     { (void)e; }
void mouse_set_cursor(cursor_type_t t)  { g_cursor_type = t; }
cursor_type_t mouse_get_cursor(void)    { return g_cursor_type; }

/* ── Compositor-visible helpers ──────────────────────────────────────────── */

bool keyboard_gui_capture_active(void) { input_state_lock(); bool v = g_gui_capture; input_state_unlock(); return v; }

int input_get_all_fds(int *buf, int maxn) {
    input_state_lock();
    int n = 0;
    for (int i = 0; i < g_kbd_cnt && n < maxn; i++) buf[n++] = g_kbd_fds[i];
    for (int i = 0; i < g_ptr_cnt && n < maxn; i++) buf[n++] = g_ptr_fds[i];
    for (int i = 0; i < g_abs_cnt && n < maxn; i++) buf[n++] = g_abs_devs[i].fd;
    for (int i = 0; i < g_gp_cnt  && n < maxn; i++) buf[n++] = g_gamepads[i].fd;
    input_state_unlock();
    return n;
}

int input_get_pointer_fds(int *buf, int maxn) {
    input_state_lock();
    int n = 0;
    if (g_libinput && maxn > 0) {
        int fd = libinput_get_fd(g_libinput);
        if (fd >= 0) buf[n++] = fd;
    } else {
        for (int i = 0; i < g_ptr_cnt && n < maxn; i++) buf[n++] = g_ptr_fds[i];
        for (int i = 0; i < g_abs_cnt && n < maxn; i++) buf[n++] = g_abs_devs[i].fd;
    }
    input_state_unlock();
    return n;
}

int input_get_control_fds(int *buf, int maxn) {
    input_state_lock();
    int n = 0;
    for (int i = 0; i < g_kbd_cnt && n < maxn; i++) buf[n++] = g_kbd_fds[i];
    for (int i = 0; i < g_gp_cnt && n < maxn; i++) buf[n++] = g_gamepads[i].fd;
    input_state_unlock();
    return n;
}
