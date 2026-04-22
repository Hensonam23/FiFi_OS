/* FiFi Gamepad Visualizer — shows live gamepad state via IPC.
 * Displays button states, D-pad, and analog stick positions.
 * Compile: gcc -O2 -static -o fifi-gamepad gamepad.c -lm
 * Launch inside FiFi after compositor is running. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define FIFI_SOCK        "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT  0x01u
#define IPC_APP_FRAME    0x02u
#define IPC_APP_CLOSE    0x04u
#define IPC_WIN_CREATED  0x10u
#define IPC_INPUT_KEY    0x11u
#define IPC_INPUT_MOUSE  0x12u
#define IPC_INPUT_GAMEPAD 0x14u
#define IPC_INVALIDATE   0x15u

/* Gamepad button bit masks (must match compositor input.c) */
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

#define WIN_W 480
#define WIN_H 320

static uint32_t g_fb[WIN_W * WIN_H];

/* Colors */
#define COL_BG      0xFF0D1117u
#define COL_PANEL   0xFF161B22u
#define COL_BORDER  0xFF30363Du
#define COL_BTN_OFF 0xFF21262Du
#define COL_BTN_ON  0xFF1F6FEBu
#define COL_A_ON    0xFF3FB950u
#define COL_B_ON    0xFFFF7B72u
#define COL_X_ON    0xFF79C0FFu
#define COL_Y_ON    0xFFE3B341u
#define COL_STICK   0xFF8B949Eu
#define COL_STICK_D 0xFF58A6FFu
#define COL_TEXT    0xFFC9D1D9u
#define COL_DIM     0xFF484F58u
#define COL_TITLE   0xFF58A6FFu

/* Current gamepad state */
static uint16_t g_btns = 0;
static int16_t  g_lx = 0, g_ly = 0;
static int16_t  g_rx = 0, g_ry = 0;
static int16_t  g_lt = 0, g_rt = 0;
static bool     g_connected = false;
static bool     g_invalidated = false;  /* IPC_INVALIDATE received — force redraw */

/* IPC partial-read state */
static uint8_t  g_hdr[8];
static int      g_hdr_got = 0;
static uint8_t *g_payload = NULL;
static uint32_t g_pld_len = 0;
static uint32_t g_pld_got = 0;

static int g_sock = -1;
static int32_t g_win_x = 0, g_win_y = 0;

/* ── Drawing primitives ──────────────────────────────────────────────────── */

static void fill_rect(int x, int y, int w, int h, uint32_t col) {
    for (int ry = y; ry < y + h; ry++) {
        if (ry < 0 || ry >= WIN_H) continue;
        for (int rx = x; rx < x + w; rx++) {
            if (rx < 0 || rx >= WIN_W) continue;
            g_fb[ry * WIN_W + rx] = col;
        }
    }
}

static void fill_circle(int cx, int cy, int r, uint32_t col) {
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx*dx + dy*dy <= r*r) {
                if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H)
                    g_fb[y * WIN_W + x] = col;
            }
        }
    }
}

static void draw_ring(int cx, int cy, int r, int thickness, uint32_t col) {
    int r2 = r - thickness;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx*dx + dy*dy;
            if (d2 <= r*r && d2 >= r2*r2) {
                if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H)
                    g_fb[y * WIN_W + x] = col;
            }
        }
    }
}

/* Tiny 6×10 bitmap font, digits + uppercase + some special chars */
static const uint8_t font6x10[][10] = {
    [' ']={'$','$','$','$','$','$','$','$','$','$'},  /* placeholder, unused */
};

/* Simpler: draw a tiny character from a 5-wide 7-tall font using pixel segments */
static void put_pixel(int x, int y, uint32_t c) {
    if (x>=0 && x<WIN_W && y>=0 && y<WIN_H) g_fb[y*WIN_W+x]=c;
}

static void draw_hline(int x, int y, int w, uint32_t c) {
    for (int i=0;i<w;i++) put_pixel(x+i,y,c);
}

static void draw_vline(int x, int y, int h, uint32_t c) {
    for (int i=0;i<h;i++) put_pixel(x,y+i,c);
}

/* 3×5 micro font — sufficient for labels */
static const uint8_t micro3x5[96][5] = {
    [' '-' ']={0,0,0,0,0},
    ['A'-' ']={0b010,0b101,0b111,0b101,0b101},
    ['B'-' ']={0b110,0b101,0b110,0b101,0b110},
    ['C'-' ']={0b011,0b100,0b100,0b100,0b011},
    ['D'-' ']={0b110,0b101,0b101,0b101,0b110},
    ['E'-' ']={0b111,0b100,0b110,0b100,0b111},
    ['F'-' ']={0b111,0b100,0b110,0b100,0b100},
    ['G'-' ']={0b011,0b100,0b101,0b101,0b011},
    ['H'-' ']={0b101,0b101,0b111,0b101,0b101},
    ['I'-' ']={0b111,0b010,0b010,0b010,0b111},
    ['J'-' ']={0b001,0b001,0b001,0b101,0b010},
    ['L'-' ']={0b100,0b100,0b100,0b100,0b111},
    ['N'-' ']={0b101,0b111,0b111,0b101,0b101},
    ['O'-' ']={0b010,0b101,0b101,0b101,0b010},
    ['P'-' ']={0b110,0b101,0b110,0b100,0b100},
    ['R'-' ']={0b110,0b101,0b110,0b101,0b101},
    ['S'-' ']={0b011,0b100,0b010,0b001,0b110},
    ['T'-' ']={0b111,0b010,0b010,0b010,0b010},
    ['U'-' ']={0b101,0b101,0b101,0b101,0b010},
    ['X'-' ']={0b101,0b101,0b010,0b101,0b101},
    ['Y'-' ']={0b101,0b101,0b010,0b010,0b010},
    ['0'-' ']={0b010,0b101,0b101,0b101,0b010},
    ['1'-' ']={0b010,0b110,0b010,0b010,0b111},
    ['2'-' ']={0b110,0b001,0b010,0b100,0b111},
    ['3'-' ']={0b110,0b001,0b010,0b001,0b110},
    ['4'-' ']={0b101,0b101,0b111,0b001,0b001},
    ['5'-' ']={0b111,0b100,0b110,0b001,0b110},
    ['6'-' ']={0b011,0b100,0b110,0b101,0b010},
    ['7'-' ']={0b111,0b001,0b010,0b010,0b010},
    ['8'-' ']={0b010,0b101,0b010,0b101,0b010},
    ['9'-' ']={0b010,0b101,0b011,0b001,0b110},
    [':'-' ']={0,0b010,0,0b010,0},
    ['-'-' ']={0,0,0b111,0,0},
    ['+'-' ']={0,0b010,0b111,0b010,0},
    ['/'-' ']={0b001,0b001,0b010,0b100,0b100},
};

static void draw_char_micro(int x, int y, char c, uint32_t col) {
    int idx = (int)c - ' ';
    if (idx < 0 || idx >= 96) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = micro3x5[idx][row];
        for (int col2 = 0; col2 < 3; col2++) {
            if (bits & (1 << (2 - col2)))
                put_pixel(x + col2, y + row, col);
        }
    }
}

static void draw_str_micro(int x, int y, const char *s, uint32_t col) {
    for (; *s; s++, x += 4)
        draw_char_micro(x, y, *s, col);
}

/* ── Game controller layout drawing ────────────────────────────────────────── */

static void draw_button(int cx, int cy, int r, bool pressed, uint32_t on_col, const char *lbl) {
    uint32_t bg = pressed ? on_col : COL_BTN_OFF;
    uint32_t border = pressed ? on_col : COL_BORDER;
    draw_ring(cx, cy, r, 2, border);
    fill_circle(cx, cy, r - 2, bg);
    if (lbl)
        draw_str_micro(cx - (int)(strlen(lbl)*2), cy - 2, lbl, pressed ? COL_BG : COL_DIM);
}

static void draw_stick(int cx, int cy, int radius, int16_t sx, int16_t sy, bool clicked) {
    /* Background circle */
    draw_ring(cx, cy, radius, 2, COL_BORDER);
    fill_circle(cx, cy, radius - 2, 0xFF0D1117u);

    /* Dot position */
    int dx = (int)sx * (radius - 6) / 32767;
    int dy = (int)sy * (radius - 6) / 32767;
    uint32_t dot_col = clicked ? 0xFFFFFFFFu : COL_STICK_D;
    fill_circle(cx + dx, cy + dy, 4, dot_col);
}

static void draw_trigger(int x, int y, int w, int h, int16_t val, const char *lbl) {
    /* Normalize 0..32767 */
    int filled = (int)((int32_t)(val + 32767) * w / 65534);
    if (filled < 0) filled = 0;
    if (filled > w) filled = w;
    fill_rect(x, y, w, h, COL_BTN_OFF);
    if (filled > 0) fill_rect(x, y, filled, h, COL_STICK_D);
    fill_rect(x, y, w, 1, COL_BORDER);
    fill_rect(x, y+h-1, w, 1, COL_BORDER);
    fill_rect(x, y, 1, h, COL_BORDER);
    fill_rect(x+w-1, y, 1, h, COL_BORDER);
    if (lbl) draw_str_micro(x + w/2 - 4, y + h/2 - 2, lbl, COL_TEXT);
}

static void draw_dpad(int cx, int cy, uint16_t btns) {
    int arm = 12;
    bool up    = !!(btns & GP_BTN_DUP);
    bool down  = !!(btns & GP_BTN_DDOWN);
    bool left  = !!(btns & GP_BTN_DLEFT);
    bool right = !!(btns & GP_BTN_DRIGHT);
    /* Center */
    fill_rect(cx - arm/2, cy - arm/2, arm, arm, COL_BORDER);
    /* Arrows */
    fill_rect(cx - arm/2, cy - arm - arm/2, arm, arm, up    ? COL_BTN_ON : COL_BTN_OFF);
    fill_rect(cx - arm/2, cy + arm/2,        arm, arm, down  ? COL_BTN_ON : COL_BTN_OFF);
    fill_rect(cx - arm - arm/2, cy - arm/2,  arm, arm, left  ? COL_BTN_ON : COL_BTN_OFF);
    fill_rect(cx + arm/2,       cy - arm/2,  arm, arm, right ? COL_BTN_ON : COL_BTN_OFF);
    /* Labels */
    draw_str_micro(cx - arm/2 + 3, cy - arm - arm/2 + 3, "U", up    ? COL_BG : COL_DIM);
    draw_str_micro(cx - arm/2 + 3, cy + arm/2 + 3,        "D", down  ? COL_BG : COL_DIM);
    draw_str_micro(cx - arm - arm/2 + 3, cy - arm/2 + 3,  "L", left  ? COL_BG : COL_DIM);
    draw_str_micro(cx + arm/2 + 3, cy - arm/2 + 3,        "R", right ? COL_BG : COL_DIM);
}

/* Draw shoulder button row */
static void draw_shoulder(int x, int y, int w, int h, bool pressed, const char *lbl) {
    uint32_t bg = pressed ? COL_BTN_ON : COL_BTN_OFF;
    fill_rect(x, y, w, h, bg);
    fill_rect(x, y, w, 1, COL_BORDER);
    fill_rect(x, y+h-1, w, 1, COL_BORDER);
    fill_rect(x, y, 1, h, COL_BORDER);
    fill_rect(x+w-1, y, 1, h, COL_BORDER);
    draw_str_micro(x + w/2 - 4, y + h/2 - 2, lbl, pressed ? COL_BG : COL_DIM);
}

static void render(void) {
    /* Background */
    fill_rect(0, 0, WIN_W, WIN_H, COL_BG);

    /* Title bar */
    fill_rect(0, 0, WIN_W, 20, COL_PANEL);
    fill_rect(0, 19, WIN_W, 1, COL_BORDER);
    draw_str_micro(8, 7, "GAMEPAD", COL_TITLE);
    const char *status = g_connected ? "CONNECTED" : "NO GAMEPAD";
    uint32_t status_col = g_connected ? 0xFF3FB950u : COL_DIM;
    draw_str_micro(WIN_W - (int)(strlen(status) * 4) - 8, 7, status, status_col);

    if (!g_connected) {
        draw_str_micro(WIN_W/2 - 40, WIN_H/2 - 4, "NO GAMEPAD CONNECTED", COL_DIM);
        draw_str_micro(WIN_W/2 - 44, WIN_H/2 + 8, "PLUG IN A CONTROLLER", COL_DIM);
        return;
    }

    /* Left side: D-pad + left stick */
    int left_cx = 100;
    int left_cy = 160;

    /* Shoulder / trigger row */
    int sh_y  = 25;
    int sh_h  = 14;
    int sh_w  = 60;
    /* LB / LT */
    draw_shoulder(10, sh_y, sh_w, sh_h, !!(g_btns & GP_BTN_LB), "LB");
    draw_trigger(10, sh_y + sh_h + 2, sh_w, 10, g_lt, "LT");
    /* RB / RT */
    draw_shoulder(WIN_W - sh_w - 10, sh_y, sh_w, sh_h, !!(g_btns & GP_BTN_RB), "RB");
    draw_trigger(WIN_W - sh_w - 10, sh_y + sh_h + 2, sh_w, 10, g_rt, "RT");

    /* SELECT / START */
    draw_button(WIN_W/2 - 30, 60, 10, !!(g_btns & GP_BTN_SELECT), COL_BTN_ON, "SEL");
    draw_button(WIN_W/2 + 30, 60, 10, !!(g_btns & GP_BTN_START),  COL_BTN_ON, "STA");

    /* D-pad */
    draw_dpad(left_cx, left_cy, g_btns);

    /* Left stick */
    int lsx = 185, lsy = 195;
    draw_stick(lsx, lsy, 30, g_lx, g_ly, !!(g_btns & GP_BTN_LS));
    draw_str_micro(lsx - 4, lsy + 36, "LS", COL_DIM);

    /* Right stick */
    int rsx = WIN_W - 185, rsy = 195;
    draw_stick(rsx, rsy, 30, g_rx, g_ry, !!(g_btns & GP_BTN_RS));
    draw_str_micro(rsx - 4, rsy + 36, "RS", COL_DIM);

    /* ABXY buttons (right side) */
    int right_cx = WIN_W - 100;
    int right_cy = 160;
    int br = 12;
    draw_button(right_cx,      right_cy - 28, br, !!(g_btns & GP_BTN_Y), COL_Y_ON, "Y");
    draw_button(right_cx,      right_cy + 28, br, !!(g_btns & GP_BTN_A), COL_A_ON, "A");
    draw_button(right_cx - 28, right_cy,       br, !!(g_btns & GP_BTN_X), COL_X_ON, "X");
    draw_button(right_cx + 28, right_cy,       br, !!(g_btns & GP_BTN_B), COL_B_ON, "B");

    /* Axis value readouts at bottom */
    char buf[16];
    int row_y = WIN_H - 18;
    draw_str_micro(8, row_y, "LX:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_lx);
    draw_str_micro(24, row_y, buf, COL_TEXT);
    draw_str_micro(64, row_y, "LY:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_ly);
    draw_str_micro(80, row_y, buf, COL_TEXT);
    draw_str_micro(120, row_y, "RX:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_rx);
    draw_str_micro(136, row_y, buf, COL_TEXT);
    draw_str_micro(176, row_y, "RY:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_ry);
    draw_str_micro(192, row_y, buf, COL_TEXT);
    draw_str_micro(232, row_y, "LT:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_lt);
    draw_str_micro(248, row_y, buf, COL_TEXT);
    draw_str_micro(288, row_y, "RT:", COL_DIM);
    snprintf(buf, sizeof(buf), "%6d", (int)g_rt);
    draw_str_micro(304, row_y, buf, COL_TEXT);
}

/* ── IPC helpers ─────────────────────────────────────────────────────────── */

static void ipc_send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    write(fd, hdr, 8);
    if (len > 0 && data) write(fd, data, len);
}

static void push_frame(int fd) {
    /* Header: x=0, y=0, w=WIN_W, h=WIN_H */
    uint8_t frame_hdr[16];
    uint32_t zero = 0, ww = WIN_W, wh = WIN_H;
    memcpy(frame_hdr,      &zero, 4);
    memcpy(frame_hdr + 4,  &zero, 4);
    memcpy(frame_hdr + 8,  &ww,   4);
    memcpy(frame_hdr + 12, &wh,   4);
    uint32_t pld_len = 16 + WIN_W * WIN_H * 4;
    uint8_t hdr[8];
    uint32_t type = IPC_APP_FRAME;
    memcpy(hdr,     &type,    4);
    memcpy(hdr + 4, &pld_len, 4);
    write(fd, hdr, 8);
    write(fd, frame_hdr, 16);
    write(fd, g_fb, WIN_W * WIN_H * 4);
}

static void dispatch_msg(uint32_t type, const uint8_t *pld, uint32_t len) {
    if (type == IPC_WIN_CREATED && len >= 20) {
        memcpy(&g_win_x, pld + 4,  4);
        memcpy(&g_win_y, pld + 8,  4);
    } else if (type == IPC_INPUT_KEY) {
        if (len >= 1 && (pld[0] == 'q' || pld[0] == 27))
            exit(0);
    } else if (type == IPC_INPUT_GAMEPAD && len >= 14) {
        memcpy(&g_btns, pld,      2);
        memcpy(&g_lx,   pld + 2,  2);
        memcpy(&g_ly,   pld + 4,  2);
        memcpy(&g_rx,   pld + 6,  2);
        memcpy(&g_ry,   pld + 8,  2);
        memcpy(&g_lt,   pld + 10, 2);
        memcpy(&g_rt,   pld + 12, 2);
        g_connected = true;
    } else if (type == IPC_INVALIDATE) {
        g_invalidated = true;
    }
}

/* Returns true if a complete message was read */
static bool ipc_read_once(int fd) {
    /* Phase 1: header */
    if (g_hdr_got < 8) {
        ssize_t n = read(fd, g_hdr + g_hdr_got, 8 - g_hdr_got);
        if (n <= 0) return false;
        g_hdr_got += (int)n;
        if (g_hdr_got < 8) return false;
        memcpy(&g_pld_len, g_hdr + 4, 4);
        g_pld_got = 0;
        if (g_pld_len > 0) {
            free(g_payload);
            g_payload = malloc(g_pld_len);
            if (!g_payload) { exit(1); }
        }
    }
    /* Phase 2: payload */
    if (g_pld_len > 0 && g_pld_got < g_pld_len) {
        ssize_t n = read(fd, g_payload + g_pld_got, g_pld_len - g_pld_got);
        if (n <= 0) return false;
        g_pld_got += (uint32_t)n;
        if (g_pld_got < g_pld_len) return false;
    }
    uint32_t type;
    memcpy(&type, g_hdr, 4);
    dispatch_msg(type, g_payload, g_pld_len);
    /* Reset */
    g_hdr_got = 0; g_pld_len = 0; g_pld_got = 0;
    free(g_payload); g_payload = NULL;
    return true;
}

int main(void) {
    /* Connect to compositor */
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);

    int retries = 20;
    while (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (--retries == 0) { perror("connect"); return 1; }
        struct timespec ts = {0, 200000000};
        nanosleep(&ts, NULL);
    }

    /* Register window */
    uint8_t conn_pld[68];
    uint16_t ww = WIN_W, wh = WIN_H;
    memcpy(conn_pld,     &ww, 2);
    memcpy(conn_pld + 2, &wh, 2);
    snprintf((char *)conn_pld + 4, 64, "Gamepad");
    ipc_send_msg(g_sock, IPC_APP_CONNECT, conn_pld, 68);

    /* Make socket non-blocking */
    fcntl(g_sock, F_SETFL, O_NONBLOCK);

    /* Wait for IPC_WIN_CREATED */
    {
        int fd2 = dup(g_sock);
        fcntl(fd2, F_SETFL, 0);
        struct pollfd pf = {fd2, POLLIN, 0};
        poll(&pf, 1, 2000);
        g_hdr_got = 0;
        ipc_read_once(fd2);
        close(fd2);
    }

    /* Main loop */
    bool needs_render = true;
    struct timespec last_render = {0, 0};

    for (;;) {
        struct pollfd pf = {g_sock, POLLIN, 0};
        poll(&pf, 1, 16);  /* ~60Hz frame rate cap */

        bool got_gamepad = false;
        while (ipc_read_once(g_sock)) {
            if (g_connected) got_gamepad = true;
        }

        if (got_gamepad || needs_render || g_invalidated) {
            render();
            push_frame(g_sock);
            needs_render = false;
            g_invalidated = false;
        }
    }

    return 0;
}
