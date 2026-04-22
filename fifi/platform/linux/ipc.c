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
#include <time.h>

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
#define IPC_INPUT_GAMEPAD 0x14u  /* {uint16_t btns; int16_t lx,ly,rx,ry,lt,rt} = 14 bytes */
#define IPC_INVALIDATE    0x15u  /* ask app to push a fresh full frame (no payload) */
#define IPC_NOTIFY        0x16u  /* app → compositor: {char text[]} — show toast notification */
#define IPC_CLIP_SET      0x17u  /* app → compositor: {char text[]} — set shared clipboard */
#define IPC_CLIP_GET      0x18u  /* app → compositor: (no payload) — request clipboard contents */
#define IPC_CLIP_DATA     0x19u  /* compositor → app: {char text[]} — clipboard contents */
#define IPC_OPEN_FILE     0x1Au  /* app → compositor: {char path[]} — open path in text viewer */

typedef struct {
    int      fd;
    bool     active;
    bool     minimized;    /* window is hidden (task button remains, process alive) */
    uint32_t win_id;    /* ID returned from gui_app_create_window() — 0 if not yet created */
    uint32_t z_order;   /* higher = on top; raised on focus */
    uint32_t win_x, win_y, win_w, win_h;
    uint32_t *frame_buf; /* cached pixel frame — win_w × win_h, repainted every tick */
    char     title[64];
    /* partial-read state */
    uint8_t  hdr[IPC_HDR_SZ];
    int      hdr_got;
    uint8_t *payload;
    uint32_t pld_len;
    uint32_t pld_got;
} ipc_client_t;

static int          g_srv_fd      = -1;
static ipc_client_t g_clients[IPC_MAX_APPS];
static int          g_focused_idx = -1;  /* which client has keyboard focus */
static uint32_t     g_next_z      = 1;   /* monotonically increasing z counter */

/* ── Toast notification state ────────────────────────────────────────────── */
#define NOTIFY_DURATION_S 3
static char    g_notify_text[80] = {0};
static time_t  g_notify_expire   = 0;   /* monotonic time when notification ends */

/* ── Shared clipboard ────────────────────────────────────────────────────── */
#define CLIP_MAX 4096
static char g_clipboard[CLIP_MAX] = {0};

/* Raise a window to the top of the z-stack */
static void ipc_raise(int i) {
    g_clients[i].z_order = g_next_z++;
}

/* ── Window drag state ────────────────────────────────────────────────────── */
#define IPC_DRAG_STRIP   24   /* top N pixels of an IPC window act as drag handle */
#define IPC_CLOSE_BTN_SZ 18   /* close button square size (in the drag strip) */
static int   g_drag_idx   = -1;   /* which client is being dragged */
static int32_t g_drag_ox  = 0;    /* cursor offset within window at drag start */
static int32_t g_drag_oy  = 0;

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

static void ipc_disconnect_client(ipc_client_t *c);  /* forward decl */

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

        /* Assign initial z-order */
        c->z_order = g_next_z++;

        /* Center the window using actual framebuffer dimensions */
        uint32_t fb_w = (uint32_t)console_fb_width();
        uint32_t fb_h = (uint32_t)console_fb_height();
        if (fb_w == 0) fb_w = 1920;
        if (fb_h == 0) fb_h = 1080;
        c->win_w = req_w;
        c->win_h = req_h;
        c->win_x = (fb_w > req_w) ? (fb_w - req_w) / 2 : 0;
        c->win_y = (fb_h > req_h) ? (fb_h - req_h) / 2 : 80;

        fprintf(stderr, "[ipc] app '%s' connected, window %ux%u at (%u,%u)\n",
                c->title, c->win_w, c->win_h, c->win_x, c->win_y);

        /* Allocate cached frame buffer — compositor repaints this every tick */
        free(c->frame_buf);
        c->frame_buf = calloc(c->win_w * c->win_h, 4);  /* zero = black until first frame */

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

        /* Cache pixels into frame_buf — ipc_blit_all() paints this every tick */
        if (!c->minimized && c->frame_buf &&
            fx + fw <= c->win_w && fy + fh <= c->win_h) {
            const uint32_t *src = (const uint32_t *)(pld + 16);
            for (uint32_t row = 0; row < fh; row++)
                memcpy(c->frame_buf + (fy + row) * c->win_w + fx,
                       src + row * fw, fw * 4);
        }
        break;
    }
    case IPC_APP_TITLE: {
        if (pld_len == 0) break;
        snprintf(c->title, sizeof(c->title), "%.*s", (int)pld_len, pld);
        fprintf(stderr, "[ipc] app title: %s\n", c->title);
        break;
    }
    case IPC_APP_CLOSE:
        ipc_disconnect_client(c);
        break;
    case IPC_NOTIFY: {
        if (pld_len > 0) {
            uint32_t len = pld_len < (uint32_t)(sizeof(g_notify_text) - 1)
                         ? pld_len : (uint32_t)(sizeof(g_notify_text) - 1);
            memcpy(g_notify_text, pld, len);
            g_notify_text[len] = '\0';
            g_notify_expire = time(NULL) + NOTIFY_DURATION_S;
            fprintf(stderr, "[ipc] notify: %s\n", g_notify_text);
        }
        break;
    }
    case IPC_CLIP_SET: {
        if (pld_len > 0) {
            uint32_t len = pld_len < (uint32_t)(CLIP_MAX - 1) ? pld_len : (uint32_t)(CLIP_MAX - 1);
            memcpy(g_clipboard, pld, len);
            g_clipboard[len] = '\0';
            fprintf(stderr, "[ipc] clipboard set (%u bytes)\n", len);
        }
        break;
    }
    case IPC_CLIP_GET: {
        uint32_t len = (uint32_t)strlen(g_clipboard);
        ipc_send(c, IPC_CLIP_DATA, g_clipboard, len);
        break;
    }
    case IPC_OPEN_FILE: {
        if (pld_len > 0 && pld_len < 512) {
            char path[512];
            memcpy(path, pld, pld_len);
            path[pld_len] = '\0';
            extern void gui_open_in_viewer(const char *path);
            gui_open_in_viewer(path);
        }
        break;
    }
    default:
        break;
    }
}

/* Disconnect a client cleanly: close socket, clear backbuffer region, release memory */
static void ipc_disconnect_client(ipc_client_t *c) {
    if (!c->active) return;
    fprintf(stderr, "[ipc] app '%s' disconnected\n", c->title);
    if (c->win_w > 0 && c->win_h > 0 && !c->minimized)
        console_fill_rect(c->win_x, c->win_y, c->win_w, c->win_h, 0x00000000u);
    close(c->fd); c->fd = -1; c->active = false;
    if (c->payload)   { free(c->payload);   c->payload   = NULL; }
    if (c->frame_buf) { free(c->frame_buf); c->frame_buf = NULL; }
    if (g_focused_idx >= 0 && &g_clients[g_focused_idx] == c) g_focused_idx = -1;
    int i = (int)(c - g_clients);
    if (g_drag_idx == i) g_drag_idx = -1;
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
                    ipc_disconnect_client(c);
                }
                return;
            }
            c->hdr_got += (int)n;
            if (c->hdr_got < IPC_HDR_SZ) return;

            /* Parse header */
            memcpy(&c->pld_len, c->hdr + 4, 4);
            if (c->pld_len > 64 * 1024 * 1024u) {
                /* Absurd payload — drop client */
                ipc_disconnect_client(c);
                return;
            }
            c->pld_got = 0;
            if (c->pld_len > 0) {
                free(c->payload);
                c->payload = malloc(c->pld_len);
                if (!c->payload) {
                    ipc_disconnect_client(c);
                    return;
                }
            }
        }

        /* Phase 2: fill payload */
        if (c->pld_len > 0 && c->pld_got < c->pld_len) {
            ssize_t n = read(c->fd, c->payload + c->pld_got, c->pld_len - c->pld_got);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    ipc_disconnect_client(c);
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

/* Close-button hit box for client c: top-right corner of drag strip */
static inline bool close_btn_hit(const ipc_client_t *c, int32_t mx, int32_t my) {
    if (c->win_w < (uint32_t)IPC_CLOSE_BTN_SZ * 2) return false;
    uint32_t sz = (uint32_t)IPC_CLOSE_BTN_SZ;
    uint32_t bx = c->win_x + c->win_w - sz - 3;
    uint32_t by = c->win_y + 3;
    return (uint32_t)mx >= bx && (uint32_t)mx < bx + sz &&
           (uint32_t)my >= by && (uint32_t)my < by + sz;
}

/* Minimize-button hit box: just left of the close button */
static inline bool min_btn_hit(const ipc_client_t *c, int32_t mx, int32_t my) {
    if (c->win_w < (uint32_t)IPC_CLOSE_BTN_SZ * 2 + 8) return false;
    uint32_t sz = (uint32_t)IPC_CLOSE_BTN_SZ;
    uint32_t bx = c->win_x + c->win_w - sz * 2 - 7;
    uint32_t by = c->win_y + 3;
    return (uint32_t)mx >= bx && (uint32_t)mx < bx + sz &&
           (uint32_t)my >= by && (uint32_t)my < by + sz;
}

/* Kill a client and clear its screen region */
static void ipc_kill_client(int i) {
    ipc_client_t *c = &g_clients[i];
    if (!c->active) return;
    /* Black out the backbuffer region so no stale pixels remain */
    if (c->win_w > 0 && c->win_h > 0)
        console_fill_rect(c->win_x, c->win_y, c->win_w, c->win_h, 0x00000000u);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->active = false;
    if (c->payload)   { free(c->payload);   c->payload   = NULL; }
    if (c->frame_buf) { free(c->frame_buf); c->frame_buf = NULL; }
    if (g_focused_idx == i) g_focused_idx = -1;
    if (g_drag_idx    == i) g_drag_idx    = -1;
    fprintf(stderr, "[ipc] closed '%s' via close button\n", c->title);
}

/* Check if click lands on any window's close or minimize button. Returns true if handled. */
bool ipc_try_close_at(int32_t mx, int32_t my) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        ipc_client_t *c = &g_clients[i];
        if (!c->active || c->fd < 0 || c->win_w == 0 || c->minimized) continue;
        if (close_btn_hit(c, mx, my)) {
            ipc_kill_client(i);
            return true;
        }
        if (min_btn_hit(c, mx, my)) {
            c->minimized = true;
            console_fill_rect(c->win_x, c->win_y, c->win_w, c->win_h, 0x00000000u);
            if (g_focused_idx == i) g_focused_idx = -1;
            fprintf(stderr, "[ipc] minimized '%s'\n", c->title);
            return true;
        }
    }
    return false;
}

/* Blit every active IPC window's cached frame into the backbuffer.
 * Call this AFTER gui_on_tick() so IPC windows appear on top of the wallpaper. */
void ipc_blit_all(void) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        ipc_client_t *c = &g_clients[i];
        if (!c->active || c->fd < 0 || c->win_w == 0 || c->minimized || !c->frame_buf) continue;
        console_paste_rect(c->frame_buf, c->win_x, c->win_y, c->win_w, c->win_h);
    }
}

/* Draw title bars and close buttons on top of all active IPC windows.
 * Called every tick from main.c after ipc_blit_all(). */
void ipc_draw_overlays(void) {
    uint64_t fw = console_font_width();
    uint64_t fh = console_font_height();

    for (int i = 0; i < IPC_MAX_APPS; i++) {
        ipc_client_t *c = &g_clients[i];
        if (!c->active || c->fd < 0 || c->win_w == 0 || c->minimized || !c->frame_buf) continue;
        if (c->win_w < (uint32_t)(IPC_CLOSE_BTN_SZ + 6)) continue;

        bool focused = (g_focused_idx == i);

        /* ── Title bar strip (top IPC_DRAG_STRIP pixels of the window) ─── */
        uint32_t tb_col = focused ? 0xFF1e3a6eu : 0xFF18283eu;
        console_fill_rect(c->win_x, c->win_y, c->win_w, (uint32_t)IPC_DRAG_STRIP, tb_col);

        /* Accent bottom edge on the title bar */
        uint32_t edge_col = focused ? 0xFF3878d8u : 0xFF243448u;
        console_fill_rect(c->win_x, c->win_y + IPC_DRAG_STRIP - 2,
                          c->win_w, 2u, edge_col);

        /* Title text (left-aligned, vertically centered in title bar) */
        if (fw > 0 && fh > 0) {
            uint64_t ty = (uint64_t)c->win_y + ((uint64_t)IPC_DRAG_STRIP - fh) / 2;
            uint64_t tx = (uint64_t)c->win_x + 8u;
            uint64_t max_tx = (uint64_t)c->win_x + c->win_w
                            - (uint64_t)IPC_CLOSE_BTN_SZ * 2u - 14u;
            for (size_t j = 0; j < sizeof(c->title) && c->title[j] && tx + fw <= max_tx;
                 j++, tx += fw)
                console_render_glyph(tx, ty,
                                     (unsigned char)c->title[j],
                                     0xFFDDE8F8u, tb_col);
        }

        /* ── Minimize button (left of close) ────────────────────────────── */
        uint32_t sz = (uint32_t)IPC_CLOSE_BTN_SZ;
        uint32_t by = c->win_y + 3;
        uint32_t min_bx = c->win_x + c->win_w - sz * 2 - 7;
        console_fill_rect(min_bx, by, sz, sz, 0xFFB09020u);
        /* Draw underscore (_) indicator */
        uint32_t mid_y = by + sz - 5;
        console_fill_rect(min_bx + 3, mid_y, sz - 6, 2, 0xFFFFFFFFu);

        /* ── Close button (right side of title bar) ─────────────────────── */
        uint32_t bx = c->win_x + c->win_w - sz - 3;
        console_fill_rect(bx, by, sz, sz, 0xFFCC2222u);

        uint32_t pad   = 3;
        uint32_t inner = sz - pad * 2;
        for (uint32_t k = 0; k < inner; k++) {
            uint32_t px1 = bx + pad + k;
            uint32_t py1 = by + pad + k;
            uint32_t px2 = bx + pad + (inner - 1 - k);
            uint32_t py2 = by + pad + k;
            if (px1 < bx + sz && py1 < by + sz)
                console_fill_rect(px1, py1, 2, 2, 0xFFFFFFFFu);
            if (px2 < bx + sz && py2 < by + sz)
                console_fill_rect(px2, py2, 2, 2, 0xFFFFFFFFu);
        }

        /* ── 1px border around the whole window ─────────────────────────── */
        uint32_t br_col = focused ? 0xFF3878d8u : 0xFF243448u;
        console_fill_rect(c->win_x,              c->win_y,              c->win_w, 1u, br_col);
        console_fill_rect(c->win_x,              c->win_y + c->win_h - 1u, c->win_w, 1u, br_col);
        console_fill_rect(c->win_x,              c->win_y,              1u, c->win_h, br_col);
        console_fill_rect(c->win_x + c->win_w - 1u, c->win_y,          1u, c->win_h, br_col);
    }
}

/* Check if mouse is over any IPC window; if so, give it focus. Returns true if hit.
 * Iterates in reverse z-order (topmost window wins), raises the hit window to front,
 * and starts a drag if the click is in the top IPC_DRAG_STRIP pixels. */
bool ipc_hit_test(int32_t mx, int32_t my) {
    /* Find the topmost (highest z_order) window under the cursor */
    int best_i = -1;
    uint32_t best_z = 0;
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        ipc_client_t *c = &g_clients[i];
        if (!c->active || c->fd < 0 || c->win_w == 0 || c->minimized) continue;
        if ((uint32_t)mx >= c->win_x && (uint32_t)mx < c->win_x + c->win_w &&
            (uint32_t)my >= c->win_y && (uint32_t)my < c->win_y + c->win_h) {
            if (c->z_order > best_z) {
                best_z = c->z_order;
                best_i = i;
            }
        }
    }
    if (best_i < 0) return false;

    if (g_focused_idx != best_i) {
        g_focused_idx = best_i;
        fprintf(stderr, "[ipc] focus → '%s'\n", g_clients[best_i].title);
    }
    /* Raise to top of z-stack and request a fresh frame so it paints over siblings */
    ipc_raise(best_i);
    ipc_send(&g_clients[best_i], IPC_INVALIDATE, NULL, 0);

    /* Start drag if click is in the top drag strip (but NOT on close/minimize button) */
    if (g_drag_idx < 0 &&
        (uint32_t)my < g_clients[best_i].win_y + IPC_DRAG_STRIP &&
        !close_btn_hit(&g_clients[best_i], mx, my) &&
        !min_btn_hit(&g_clients[best_i], mx, my)) {
        g_drag_idx = best_i;
        g_drag_ox  = mx - (int32_t)g_clients[best_i].win_x;
        g_drag_oy  = my - (int32_t)g_clients[best_i].win_y;
    }
    return true;
}

/* Update drag position; returns true if a drag is active (caller should not send mouse events). */
bool ipc_drag_update(int32_t mx, int32_t my, bool lbtn) {
    if (!lbtn) {
        g_drag_idx = -1;
        return false;
    }
    if (g_drag_idx < 0) return false;
    ipc_client_t *c = &g_clients[g_drag_idx];
    if (!c->active || c->fd < 0) { g_drag_idx = -1; return false; }
    int32_t new_x = mx - g_drag_ox;
    int32_t new_y = my - g_drag_oy;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if ((uint32_t)new_x != c->win_x || (uint32_t)new_y != c->win_y) {
        /* Clear old window position in backbuffer before moving */
        console_fill_rect(c->win_x, c->win_y, c->win_w, c->win_h, 0x00000000u);
        c->win_x = (uint32_t)new_x;
        c->win_y = (uint32_t)new_y;
    }
    return true;
}

bool ipc_keyboard_active(void) {
    return g_focused_idx >= 0 &&
           g_clients[g_focused_idx].active &&
           g_clients[g_focused_idx].fd >= 0;
}

/* Send a key event to the focused app */
void ipc_send_focused_key(uint8_t key) {
    if (!ipc_keyboard_active()) return;
    ipc_send(&g_clients[g_focused_idx], IPC_INPUT_KEY, &key, 1);
}

/* Send mouse event (absolute screen coords) to the focused app translated to
 * window-relative coords. */
void ipc_send_focused_mouse(int32_t mx, int32_t my, uint8_t btns) {
    if (!ipc_keyboard_active()) return;
    ipc_client_t *c = &g_clients[g_focused_idx];
    int32_t rx = mx - (int32_t)c->win_x;
    int32_t ry = my - (int32_t)c->win_y;
    uint8_t buf[9];
    memcpy(buf,     &rx,  4);
    memcpy(buf + 4, &ry,  4);
    buf[8] = btns;
    ipc_send(c, IPC_INPUT_MOUSE, buf, sizeof(buf));
}

/* Clear IPC keyboard focus (compositor GUI reclaimed focus) */
void ipc_clear_focus(void) { g_focused_idx = -1; }

/* Keep old API for any callers */
void ipc_send_key(uint32_t focused_win_id, uint8_t key) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (g_clients[i].active && g_clients[i].win_id == focused_win_id) {
            ipc_send(&g_clients[i], IPC_INPUT_KEY, &key, 1);
            return;
        }
    }
}

/* Broadcast gamepad state to the focused app (called from main loop) */
void ipc_send_gamepad(uint16_t btns, int16_t lx, int16_t ly,
                      int16_t rx, int16_t ry, int16_t lt, int16_t rt) {
    if (!ipc_keyboard_active()) return;
    uint8_t buf[14];
    memcpy(buf,      &btns, 2);
    memcpy(buf + 2,  &lx,   2);
    memcpy(buf + 4,  &ly,   2);
    memcpy(buf + 6,  &rx,   2);
    memcpy(buf + 8,  &ry,   2);
    memcpy(buf + 10, &lt,   2);
    memcpy(buf + 12, &rt,   2);
    ipc_send(&g_clients[g_focused_idx], IPC_INPUT_GAMEPAD, buf, sizeof(buf));
}

/* Returns the IPC server fd for inclusion in the main poll set */
int ipc_server_fd(void) { return g_srv_fd; }

/* Draw active notification toast in bottom-right corner (above taskbar).
 * Returns true if it dirtied the backbuffer (caller should force flip). */
bool ipc_notify_draw(void) {
    uint64_t fb_w = console_fb_width();
    uint64_t fb_h = console_fb_height();
    uint64_t fw   = console_font_width();
    uint64_t fh   = console_font_height();

#define NOTIFY_TASKBAR_H  32u
#define NOTIFY_PAD_X      10u
#define NOTIFY_PAD_Y      6u

    /* Use a saved position so we can clear it consistently */
    static uint64_t s_bx = 0, s_by = 0, s_bw = 0, s_bh = 0;

    if (g_notify_text[0] == '\0') return false;

    if (time(NULL) >= g_notify_expire) {
        /* Expired — clear the notification area with desktop background */
        if (s_bw > 0 && s_bh > 0) {
            console_fill_rect(s_bx, s_by, s_bw, s_bh, 0x001a1a2eu);
            s_bw = 0; s_bh = 0;
        }
        g_notify_text[0] = '\0';
        return true;  /* one more flip to clear */
    }

    /* Measure text */
    size_t tlen = strlen(g_notify_text);
    if (tlen > 60) tlen = 60;
    uint64_t tw = (uint64_t)tlen * fw;
    uint64_t bw = tw + NOTIFY_PAD_X * 2;
    uint64_t bh = fh + NOTIFY_PAD_Y * 2;

    /* Position: bottom-right, above taskbar */
    uint64_t bx = fb_w > bw + 8u ? fb_w - bw - 8u : 0u;
    uint64_t by = fb_h > NOTIFY_TASKBAR_H + bh + 8u
                ? fb_h - NOTIFY_TASKBAR_H - bh - 8u : 0u;

    s_bx = bx; s_by = by; s_bw = bw; s_bh = bh;

    /* Dark background with accent border */
    console_fill_rect(bx,           by,           bw, bh, 0xFF101828u);
    console_fill_rect(bx,           by,           bw, 2u, 0xFF3878d8u);
    console_fill_rect(bx,           by + bh - 2u, bw, 2u, 0xFF3878d8u);
    console_fill_rect(bx,           by,           2u, bh, 0xFF3878d8u);
    console_fill_rect(bx + bw - 2u, by,           2u, bh, 0xFF3878d8u);

    /* Text */
    uint64_t tx = bx + NOTIFY_PAD_X;
    uint64_t ty = by + NOTIFY_PAD_Y;
    for (size_t i = 0; i < tlen; i++)
        console_render_glyph(tx + i * fw, ty,
                             (unsigned char)g_notify_text[i],
                             0xFFE0E8F0u, 0xFF101828u);
    return true;
}

/* ── Window list query API (called from gui.c taskbar) ──────────────────── */

int ipc_window_count(void) {
    int n = 0;
    for (int i = 0; i < IPC_MAX_APPS; i++)
        if (g_clients[i].active && g_clients[i].fd >= 0 && g_clients[i].win_w > 0) n++;
    return n;
}

/* Fill title (truncated to title_max-1), focused flag, and minimized flag for nth window.
 * Returns false if slot is out of range. (title_max >= 2: title[] gets an extra '~' prefix
 * if minimized, so callers can see the state from the title alone if preferred.) */
bool ipc_window_info(int slot, char *title, int title_max, bool *focused) {
    int found = 0;
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (!g_clients[i].active || g_clients[i].fd < 0 || g_clients[i].win_w == 0)
            continue;
        if (found == slot) {
            if (title && title_max > 0) {
                int len = (int)strlen(g_clients[i].title);
                if (len >= title_max) len = title_max - 1;
                memcpy(title, g_clients[i].title, (size_t)len);
                title[len] = '\0';
            }
            if (focused) *focused = (g_focused_idx == i && !g_clients[i].minimized);
            return true;
        }
        found++;
    }
    return false;
}

/* Toggle minimize or raise/focus the nth active IPC window */
void ipc_window_focus_slot(int slot) {
    int found = 0;
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (!g_clients[i].active || g_clients[i].fd < 0 || g_clients[i].win_w == 0)
            continue;
        if (found == slot) {
            ipc_client_t *c = &g_clients[i];
            if (!c->minimized && g_focused_idx == i) {
                /* Already focused → minimize */
                c->minimized = true;
                console_fill_rect(c->win_x, c->win_y, c->win_w, c->win_h, 0x00000000u);
                if (g_focused_idx == i) g_focused_idx = -1;
                fprintf(stderr, "[ipc] minimized '%s'\n", c->title);
            } else {
                /* Restore / raise */
                c->minimized = false;
                g_focused_idx = i;
                ipc_raise(i);
                ipc_send(c, IPC_INVALIDATE, NULL, 0);
                fprintf(stderr, "[ipc] restored '%s'\n", c->title);
            }
            return;
        }
        found++;
    }
}

void ipc_shutdown(void) {
    for (int i = 0; i < IPC_MAX_APPS; i++) {
        if (g_clients[i].fd >= 0) {
            close(g_clients[i].fd);
            free(g_clients[i].payload);
            free(g_clients[i].frame_buf);
        }
    }
    if (g_srv_fd >= 0) {
        close(g_srv_fd);
        unlink(FIFI_SOCK);
        g_srv_fd = -1;
    }
}
