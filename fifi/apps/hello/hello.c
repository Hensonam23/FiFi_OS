/* FiFi Hello App — minimal IPC demo.
 * Connects to the FiFi compositor socket, registers a window, and
 * paints a solid colored rectangle with a simple greeting string.
 * Compile: gcc -O2 -o hello hello.c
 * Run from inside the FiFi initramfs after compositor is up. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define FIFI_SOCK       "/tmp/fifi-compositor.sock"
#define IPC_APP_CONNECT 0x01u
#define IPC_APP_FRAME   0x02u
#define IPC_APP_CLOSE   0x04u
#define IPC_WIN_CREATED 0x10u
#define IPC_INPUT_KEY   0x11u

#define WIN_W 320
#define WIN_H 180

static void send_msg(int fd, uint32_t type, const void *data, uint32_t len) {
    uint8_t hdr[8];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    write(fd, hdr, 8);
    if (len > 0 && data) write(fd, data, len);
}

static uint32_t col_bg  = 0xFF1A1A2Eu;   /* dark navy */
static uint32_t col_txt = 0xFFE0E0FFu;   /* light text */

/* Tiny 5×7 ASCII font — enough for the greeting */
static const uint8_t font5x7[128][7] = {
    ['H'] = {0x88,0x88,0xF8,0x88,0x88,0x00,0x00},
    ['e'] = {0x00,0x70,0x88,0xF8,0x80,0x70,0x00},
    ['l'] = {0xC0,0x40,0x40,0x40,0x40,0xE0,0x00},
    ['o'] = {0x00,0x70,0x88,0x88,0x88,0x70,0x00},
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['F'] = {0xF8,0x80,0xF0,0x80,0x80,0x80,0x00},
    ['i'] = {0x40,0x00,0xC0,0x40,0x40,0xE0,0x00},
    ['!'] = {0x40,0x40,0x40,0x40,0x00,0x40,0x00},
    ['P'] = {0xF0,0x88,0xF0,0x80,0x80,0x80,0x00},
    ['r'] = {0x00,0xB0,0xC8,0x80,0x80,0x80,0x00},
    ['s'] = {0x00,0x70,0x80,0x70,0x08,0xF0,0x00},
    ['t'] = {0x40,0xE0,0x40,0x40,0x40,0x38,0x00},
    ['a'] = {0x00,0x60,0x08,0x78,0x88,0x78,0x00},
    ['p'] = {0x00,0xF0,0x88,0xF0,0x80,0x80,0x00},
    ['I'] = {0xE0,0x40,0x40,0x40,0x40,0xE0,0x00},
    ['C'] = {0x78,0x80,0x80,0x80,0x80,0x78,0x00},
};

static void draw_char(uint32_t *fb, int fw, char c, int x, int y, uint32_t fg) {
    if ((unsigned)c >= 128) return;
    for (int row = 0; row < 7; row++) {
        uint8_t bits = font5x7[(unsigned)c][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (0x80u >> col)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < fw && py >= 0 && py < WIN_H)
                    fb[py * WIN_W + px] = fg;
            }
        }
    }
}

static void draw_text(uint32_t *fb, const char *s, int x, int y, uint32_t fg) {
    for (; *s; s++, x += 6)
        draw_char(fb, WIN_W, *s, x, y, fg);
}

int main(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    printf("[hello] connected to compositor\n");

    /* Register window */
    uint8_t conn[68] = {0};
    uint16_t w = WIN_W, h = WIN_H;
    memcpy(conn,     &w, 2);
    memcpy(conn + 2, &h, 2);
    snprintf((char *)(conn + 4), 64, "Hello World");
    send_msg(fd, IPC_APP_CONNECT, conn, sizeof(conn));

    /* Wait for WIN_CREATED response */
    uint8_t hdr[8] = {0};
    read(fd, hdr, 8);
    uint32_t type, plen;
    memcpy(&type,  hdr,     4);
    memcpy(&plen,  hdr + 4, 4);
    if (type == IPC_WIN_CREATED && plen >= 20) {
        uint8_t resp[20]; read(fd, resp, 20);
        printf("[hello] window created at compositor\n");
    }

    /* Paint a frame */
    uint32_t *pixels = malloc(WIN_W * WIN_H * 4);
    if (!pixels) { close(fd); return 1; }

    /* Background */
    for (int i = 0; i < WIN_W * WIN_H; i++) pixels[i] = col_bg;

    /* Simple border */
    for (int x = 0; x < WIN_W; x++) {
        pixels[x]                    = col_txt;
        pixels[(WIN_H - 1) * WIN_W + x] = col_txt;
    }
    for (int y = 0; y < WIN_H; y++) {
        pixels[y * WIN_W]             = col_txt;
        pixels[y * WIN_W + WIN_W - 1] = col_txt;
    }

    draw_text(pixels, "Hello FiFi!", 16, WIN_H / 2 - 10, col_txt);
    draw_text(pixels, "IPC App Protocol", 16, WIN_H / 2 + 5, 0xFF8080FFu);

    /* Send frame to compositor */
    uint32_t frm_hdr[4] = {0, 0, WIN_W, WIN_H};
    uint32_t total = 16 + WIN_W * WIN_H * 4;
    uint8_t *msg = malloc(total);
    memcpy(msg,      frm_hdr, 16);
    memcpy(msg + 16, pixels, WIN_W * WIN_H * 4);
    send_msg(fd, IPC_APP_FRAME, msg, total);
    free(msg); free(pixels);

    printf("[hello] frame sent — press Enter to close\n");

    /* Wait for keypress or disconnect */
    uint8_t buf[16];
    read(fd, buf, sizeof(buf));

    send_msg(fd, IPC_APP_CLOSE, NULL, 0);
    close(fd);
    return 0;
}
