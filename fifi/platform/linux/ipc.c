/* FiFi compositor IPC — Unix socket server.
 * Apps connect to FIFI_SOCK, register a window, push pixel frames,
 * and receive key/mouse input events from the compositor.
 *
 * Protocol: fixed 8-byte header + variable payload.
 *   [uint32_t type][uint32_t payload_len][payload...]
 *
 * Message types (compositor ← app):
 *   IPC_APP_CONNECT   — app registration: {uint16_t w, h; char title[60]}
 *   IPC_APP_FRAME     — pixel data: {uint32_t x, y, w, h; uint32_t pixels[w*h]}
 *   IPC_APP_TITLE     — update title: {char title[64]}
 *   IPC_APP_CLOSE     — app is closing (no payload)
 *
 * Message types (compositor → app):
 *   IPC_WIN_CREATED   — window info: {uint32_t id, x, y, w, h}
 *   IPC_INPUT_KEY     — {uint8_t key}
 *   IPC_INPUT_MOUSE   — {int32_t x, y; uint8_t buttons}
 *   IPC_FOCUS         — {uint8_t focused}
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <poll.h>

#include "console.h"
#include "gui.h"

#define FIFI_SOCK     "/tmp/fifi-compositor.sock"
#define IPC_MAX_APPS  8
#define IPC_HDR_SZ    8        /* uint32_t type + uint32_t len */
#define IPC_MAX_PLD   (4096 * 4096 * 4 + 64)   /* worst-case full-screen frame */

/* Message type IDs */
#define IPC_APP_CONNECT   0x01u
#define IPC_APP_FRAME     0x02u
#define IPC_APP_TITLE     0x03u
#define IPC_APP_CLOSE     0x04u
#define IPC_WIN_CREATED   0x10u
#define IPC_INPUT_KEY     0x11u
#define IPC_INPUT_MOUSE   0x12u
#define IPC_FOCUS         0x13u

typedef struct {
    int      fd;
    bool     active;
    uint32_t win_id;    /* ID returned from gui_app_create_window() — 0 if not yet created */
    uint32_t win_x, win_y, win_w, win_h;
    char     title[64];
    /* partial-read state */
    uint8_t  hdr[IPC_HDR_SZ];
    int      hdr_got;
    uint8_t *payload;
    uint32_t pld_len;
    uint32_t pld_got;
} ipc_client_t;

static int          g_srv_fd   = -1;
static ipc_client_t g_clients[IPC_MAX_APPS];

/* ── Send a message to an app ────────────────────────────────────────────── */
static void ipc_send(ipc_client_t *c, uint32_t type, const void *data, uint32_t len) {
    if (c->fd < 0) return;
    uint8_t hdr[IPC_HDR_SZ];
    memcpy(hdr,     &type, 4);
    memcpy(hdr + 4, &len,  4);
    /* Best-effort writes — if they fail the app disconnected */
    if (write(c->fd, hdr, IPC_HDR_SZ) < 0) return;
    if (len > 0 && data) write(c->fd, data, len);
}

/* ── Dispatch a fully-received message from an app ──────────────────────── */
static void ipc_dispatch(ipc_client_t *c, uint32_t type,
                         const uint8_t *pld, uint32_t pld_len) {
    switch (type) {
    case IPC_APP_CONNECT: {
        if (pld_len < 4) break;
        uint16_t req_w, req_h;
        memcpy(&req_w, pld,     2);
        memcpy(&req_h, pld + 2, 2);
        const char *title = pld_len > 4 ? (const char *)(pld + 4) : "App";
        snprintf(c->title, sizeof(c->title), "%s", title);

        /* Clamp window size to 1920×1080 if larger */
        if (req_w > 1920) req_w = 1920;
        if (req_h > 1080) req_h = 1080;

        /* Center the window — gui_get_screen_size() gives framebuffer dims */
        uint32_t fb_w = 1920, fb_h = 1080;  /* TODO: query from console */
        c->win_w = req_w;
        c->win_h = req_h;
        c->win_x = (fb_w > req_w) ? (fb_w - req_w) / 2 : 0;
        c->win_y = (fb_h > req_h) ? (fb_h - req_h) / 2 : 80;

        fprintf(stderr, "[ipc] app '%s' connected, window %ux%u at (%u,%u)\n",
                c->title, c->win_w, c->win_h, c->win_x, c->win_y);

        /* Reply with window info */
        uint32_t resp[5] = {
            c->win_id, c->win_x, c->win_y, c->win_w, c->win_h
        };
        ipc_send(c, IPC_WIN_CREATED, resp, sizeof(resp));
        break;
    }
    case IPC_APP_FRAME: {
        /* {uint32_t x, y, w, h} + pixel data */
        if (pld_len < 16) break;
        uint32_t fx, fy, fw, fh;
        memcpy(&fx, pld,      4);
        memcpy(&fy, pld + 4,  4);
        memcpy(&fw, pld + 8,  4);
        memcpy(&fh, pld + 12, 4);
        uint32_t expected = 16 + fw * fh * 4;
        if (pld_len < expected || fw == 0 || fh == 0) break;

        /* Blit into backbuffer at the app window's compositor position */
        const uint32_t *pixels = (const uint32_t *)(pld + 16);
        uint64_t dst_x = (uint64_t)c->win_x + fx;
        uint64_t dst_y = (uint64_t)c->win_y + fy;
        console_paste_rect(pixels, dst_x, dst_y, fw, fh);
        break;
    }
    case IPC_APP_TITLE: {
        if (pld_len == 0) break;
        snprintf(c->title, sizeof(c->title), "%.*s", (int)pld_len, pld);
        fprintf(stderr, "[ipc] app title: %s\n", c->title);
        break;
    }
    case IPC_APP_CLOSE:
        fprintf(stderr, "[ipc] app '%s' closed\n", c->title);
        close(c->fd);
        c->fd     = -1;
        c->active = false;
        if (c->payload) { free(c->payload); c->payload = NULL; }
        break;
    default:
        break;
    }
}

/* ── Read available data from one client ─────────────────────────────────── */
static void ipc_read_client(ipc_client_t *c) {
    for (;;) {
        /* Phase 1: fill header */
        if (c->hdr_got < IPC_HDR_SZ) {
            ssize_t n = read(c->fd, c->hdr + c->hdr_got, IPC_HDR_SZ - c->hdr_got);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    /* Disconnected */
                    close(c->fd); c->fd = -1; c->active = false;
                    if (c->payload) { free(c->payload); c->payload = NULL; }
                }
                return;
            }
            c->hdr_got += (int)n;
            if (c->hdr_got < IPC_HDR_SZ) return;

            /* Parse header */
            memcpy(&c->pld_len, c->hdr + 4, 4);
            if (c->pld_len > 64 * 1024 * 1024u) {
                /* Absurd payload — drop client */
                close(c->fd); c->fd = -1; c->active = false;
                return;
            }
            c->pld_got = 0;
            if (c->pld_len > 0) {
                free(c->payload);
                c->payload = malloc(c->pld_len);
                if (!c->payload) {
                    close(c->fd); c->fd = -1; c->active = false;
                    return;
                }
            }
        }

        /* Phase 2: fill payload */
        if (c->pld_len > 0 && c->pld_got < c->pld_len) {
            ssize_t n = read(c->fd, c->payload + c->pld_got, c->pld_len - c->pld_got);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(c->fd); c->fd = -1; c->active = false;
                    if (c->payload) { free(c->payload); c->payload = NULL; }
                }
                return;
            }
            c->pld_got += (uint32_t)n;
            if (c->pld_got < c->pld_len) return;
        }

        /* Full message received */
        uint32_t type;
        memcpy(&type, c->hdr, 4);
        ipc_dispatch(c, type, c->payload, c->pld_len);

        /* Reset for next message */
        c->hdr_got = 0;
        c->pld_len = 0;
        c->pld_got = 0;
        free(c->payload);
        c->payload = NULL;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ipc_init(void) {
    unlink(FIFI_SOCK);

    g_srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_srv_fd < 0) {
        fprintf(stderr, "[ipc] socket failed: %d\n", errno);
        return;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FIFI_SOCK, sizeof(addr.sun_path) - 1);

    if (bind(g_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(g_srv_fd, IPC_MAX_APPS) < 0) {
        fprintf(stderr, "[ipc] bind/listen failed: %d\n", errno);
        close(g_srv_fd);
        g_srv_fd = -1;
        return;
    }

    for (int i = 0; i < IPC_MAX_APPS; i++) {
        g_clients[i].fd     = -1;
        g_clients[i].active = false;
    }

    fprintf(stderr, "[ipc] listening on %s\n", FIFI_SOCK);
}

/* Call this every compositor tick — accepts new connections, reads messages. */
void ipc_poll(void) {
    if (g_srv_fd < 0) return;

    /* Accept new connections */
    for (;;) {
        int fd = accept4(g_srv_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        bool accepted = false;
        for (int i = 0; i < IPC_MAX_APPS; i++) {
            if (!g_clients[i].active) {
                memset(&g_clients[i], 0, sizeof(g_clients[i]));
                g_clients[i].fd     = fd;
                g_clients[i].active = true;
                g_clients[i].hdr_got = 0;
                fprintf(stderr, "[ipc] new app client (slot %d)\n", i);
                accepted = true;
                break;
            }
        }
        if (!accepted) {
            fprintf(stderr, "[ipc] max clients — refusing connection\n");
            close(fd);
        }
    }

    /* Read from all active clients */
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (g_clients[i].active && g_clients[i].fd >= 0)
            ipc_read_client(&g_clients[i]);
    }
}

/* Send a key event to the focused app (call from keyboard routing) */
void ipc_send_key(uint32_t focused_win_id, uint8_t key) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (g_clients[i].active && g_clients[i].win_id == focused_win_id) {
            ipc_send(&g_clients[i], IPC_INPUT_KEY, &key, 1);
            return;
        }
    }
}

/* Returns the IPC server fd for inclusion in the main poll set */
int ipc_server_fd(void) { return g_srv_fd; }

void ipc_shutdown(void) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (g_clients[i].fd >= 0) {
            close(g_clients[i].fd);
            free(g_clients[i].payload);
        }
    }
    if (g_srv_fd >= 0) {
        close(g_srv_fd);
        unlink(FIFI_SOCK);
        g_srv_fd = -1;
    }
}
