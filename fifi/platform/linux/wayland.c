/* wayland.c — Minimal Wayland compositor for FiFi OS.
 *
 * Implements the Wayland wire protocol over a Unix-domain socket at
 * $XDG_RUNTIME_DIR/wayland-0 (or /tmp/wayland-0 as fallback).
 *
 * Globals advertised to clients:
 *   wl_compositor  v4   — create surfaces
 *   wl_shm         v1   — shared-memory pixel buffers
 *   wl_seat        v7   — keyboard + pointer input
 *   wl_output      v4   — display geometry
 *   xdg_wm_base    v3   — XDG Shell toplevel windows
 *   wl_data_device_manager v3 — clipboard / drag-and-drop
 *
 * Each connected client gets a wl_client; surfaces are rendered by
 * calling ipc_blit_wayland() so they appear on top of the FiFi GUI.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>

/* ── Wire protocol constants ─────────────────────────────────────────────── */

/* wl_display (object 1) opcodes */
#define WL_DISPLAY_ERROR          0   /* event */
#define WL_DISPLAY_DELETE_ID      1   /* event */
#define WL_DISPLAY_SYNC           0   /* request */
#define WL_DISPLAY_GET_REGISTRY   1   /* request */

/* wl_registry opcodes */
#define WL_REGISTRY_GLOBAL        0   /* event */
#define WL_REGISTRY_GLOBAL_REMOVE 1   /* event */
#define WL_REGISTRY_BIND          0   /* request */

/* wl_callback opcodes */
#define WL_CALLBACK_DONE          0   /* event */

/* wl_compositor opcodes */
#define WL_COMPOSITOR_CREATE_SURFACE     0   /* request */
#define WL_COMPOSITOR_CREATE_REGION      1   /* request */

/* wl_surface opcodes */
#define WL_SURFACE_DESTROY        0
#define WL_SURFACE_ATTACH         1
#define WL_SURFACE_DAMAGE         2
#define WL_SURFACE_FRAME          3
#define WL_SURFACE_SET_OPAQUE_RGN 4
#define WL_SURFACE_SET_INPUT_RGN  5
#define WL_SURFACE_COMMIT         6
#define WL_SURFACE_SET_BUFFER_TRANSFORM 7
#define WL_SURFACE_SET_BUFFER_SCALE 8
#define WL_SURFACE_DAMAGE_BUFFER  9
/* events */
#define WL_SURFACE_ENTER          0
#define WL_SURFACE_LEAVE          1

/* wl_shm opcodes */
#define WL_SHM_FORMAT             0   /* event */
#define WL_SHM_CREATE_POOL        0   /* request */
#define WL_SHM_FORMAT_ARGB8888    0
#define WL_SHM_FORMAT_XRGB8888    1

/* wl_shm_pool opcodes */
#define WL_SHM_POOL_CREATE_BUFFER 0
#define WL_SHM_POOL_DESTROY       1
#define WL_SHM_POOL_RESIZE        2

/* wl_buffer opcodes */
#define WL_BUFFER_DESTROY         0
#define WL_BUFFER_RELEASE         0   /* event */

/* wl_seat opcodes */
#define WL_SEAT_CAPABILITIES      0   /* event */
#define WL_SEAT_NAME              1   /* event */
#define WL_SEAT_GET_POINTER       0
#define WL_SEAT_GET_KEYBOARD      1
#define WL_SEAT_GET_TOUCH         2
#define WL_SEAT_CAP_POINTER       (1u<<0)
#define WL_SEAT_CAP_KEYBOARD      (1u<<1)

/* wl_keyboard opcodes */
#define WL_KBD_KEYMAP             0   /* event */
#define WL_KBD_ENTER              1   /* event */
#define WL_KBD_LEAVE              2   /* event */
#define WL_KBD_KEY                3   /* event */
#define WL_KBD_MODIFIERS          4   /* event */
#define WL_KBD_REPEAT_INFO        5   /* event */
#define WL_KBD_KEYMAP_FORMAT_XKB  1

/* wl_pointer opcodes (events) */
#define WL_PTR_ENTER              0
#define WL_PTR_LEAVE              1
#define WL_PTR_MOTION             2
#define WL_PTR_BUTTON             3
#define WL_PTR_AXIS               4
#define WL_PTR_FRAME              5

/* wl_output opcodes */
#define WL_OUTPUT_GEOMETRY        0   /* event */
#define WL_OUTPUT_MODE            1   /* event */
#define WL_OUTPUT_DONE            2   /* event */
#define WL_OUTPUT_SCALE           3   /* event */

/* xdg_wm_base opcodes */
#define XDG_WM_BASE_PING          0   /* event */
#define XDG_WM_BASE_DESTROY       0
#define XDG_WM_BASE_CREATE_POSITIONER 1
#define XDG_WM_BASE_GET_XDG_SURFACE 2
#define XDG_WM_BASE_PONG          3

/* xdg_surface opcodes */
#define XDG_SURFACE_CONFIGURE     0   /* event */
#define XDG_SURFACE_DESTROY       0
#define XDG_SURFACE_GET_TOPLEVEL  1
#define XDG_SURFACE_GET_POPUP     2
#define XDG_SURFACE_SET_WINDOW_GEOMETRY 3
#define XDG_SURFACE_ACK_CONFIGURE 4

/* xdg_toplevel opcodes */
#define XDG_TOPLEVEL_CONFIGURE    0   /* event */
#define XDG_TOPLEVEL_CLOSE        1   /* event */
#define XDG_TOPLEVEL_DESTROY      0
#define XDG_TOPLEVEL_SET_PARENT   1
#define XDG_TOPLEVEL_SET_TITLE    2
#define XDG_TOPLEVEL_SET_APP_ID   3
#define XDG_TOPLEVEL_SHOW_WINDOW_MENU 4
#define XDG_TOPLEVEL_MOVE         5
#define XDG_TOPLEVEL_RESIZE       6
#define XDG_TOPLEVEL_SET_MAX_SIZE 7
#define XDG_TOPLEVEL_SET_MIN_SIZE 8
#define XDG_TOPLEVEL_SET_MAXIMIZED 9
#define XDG_TOPLEVEL_UNSET_MAXIMIZED 10
#define XDG_TOPLEVEL_SET_FULLSCREEN 11
#define XDG_TOPLEVEL_UNSET_FULLSCREEN 12

/* ── Object ID allocation ────────────────────────────────────────────────── */
/* IDs 1..WL_PREALLOC_MAX are server-assigned (display=1, etc.)
 * IDs > that are client-assigned. */
#define WL_PREALLOC_MAX  0x00ffffffu

/* ── Per-object type tags ─────────────────────────────────────────────────── */
typedef enum {
    OBJ_NONE = 0,
    OBJ_DISPLAY,
    OBJ_REGISTRY,
    OBJ_CALLBACK,
    OBJ_COMPOSITOR,
    OBJ_SURFACE,
    OBJ_REGION,
    OBJ_SHM,
    OBJ_SHM_POOL,
    OBJ_BUFFER,
    OBJ_SEAT,
    OBJ_POINTER,
    OBJ_KEYBOARD,
    OBJ_OUTPUT,
    OBJ_XDG_WM_BASE,
    OBJ_XDG_SURFACE,
    OBJ_XDG_TOPLEVEL,
    OBJ_DATA_DEVICE_MGR,
    OBJ_DATA_SOURCE,
    OBJ_DATA_DEVICE,
    OBJ_DATA_OFFER,
} obj_type_t;

/* ── Wayland object table ─────────────────────────────────────────────────── */
#define MAX_OBJECTS  512

typedef struct wl_client wl_client_t;

typedef struct {
    obj_type_t type;
    uint32_t   id;      /* Wayland object ID (1-based) */
    void      *data;    /* points into client's object data pool */
} wl_obj_t;

/* ── Shared-memory buffer ─────────────────────────────────────────────────── */
typedef struct {
    void    *data;      /* mmap'd shm area */
    size_t   size;
    int32_t  width, height, stride;
    uint32_t format;
    int      fd;
    bool     released;  /* compositor has released it */
} wl_shm_buf_t;

/* ── Surface ──────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t     buffer_id;   /* 0 = no buffer attached */
    wl_shm_buf_t *buf;        /* current committed buffer */
    int32_t      x, y;        /* position on screen */
    int32_t      w, h;
    char         title[128];
    bool         mapped;      /* has been committed at least once with a buffer */
    uint32_t     xdg_surface_id;
    uint32_t     xdg_toplevel_id;
    uint32_t     output_id;   /* wl_output this surface is on */
    uint32_t     serial;      /* xdg configure serial */
} wl_surface_t;

/* ── Client ───────────────────────────────────────────────────────────────── */
#define WL_RECV_BUF 65536
#define WL_SEND_BUF 65536
#define MAX_OBJS_PER_CLIENT 512

struct wl_client {
    int       fd;
    bool      active;
    uint8_t   recv[WL_RECV_BUF];
    int       recv_used;
    uint8_t   send[WL_SEND_BUF];
    int       send_used;
    wl_obj_t  objs[MAX_OBJS_PER_CLIENT];
    int       n_objs;
    uint32_t  serial;   /* next event serial */
    /* per-client global object IDs (server-assigned when client binds) */
    uint32_t  compositor_id;
    uint32_t  shm_id;
    uint32_t  seat_id;
    uint32_t  keyboard_id;
    uint32_t  pointer_id;
    uint32_t  output_id;
    uint32_t  xdg_wm_id;
};

/* ── Server state ─────────────────────────────────────────────────────────── */
#define MAX_WL_CLIENTS 8
static int         g_wl_fd      = -1;   /* listening socket */
static wl_client_t g_wl_clients[MAX_WL_CLIENTS];
static uint32_t    g_global_serial = 1;
static int         g_w = 1024, g_h = 768;  /* display size — set from framebuffer */
static char        g_sock_path[128] = {0};

/* ── Wire protocol helpers ───────────────────────────────────────────────── */

/* Append a 32-bit word to the send buffer */
static void wl_push_u32(wl_client_t *c, uint32_t v) {
    if (c->send_used + 4 > WL_SEND_BUF) return;
    memcpy(c->send + c->send_used, &v, 4);
    c->send_used += 4;
}

/* Append a Wayland string (length-prefixed, NUL-terminated, 4-aligned) */
static void wl_push_str(wl_client_t *c, const char *s) {
    uint32_t slen = s ? (uint32_t)strlen(s) + 1 : 0;
    uint32_t pad  = (4 - (slen % 4)) % 4;
    wl_push_u32(c, slen);
    if (c->send_used + (int)(slen + pad) > WL_SEND_BUF) return;
    if (slen) { memcpy(c->send + c->send_used, s, slen); c->send_used += slen; }
    for (uint32_t i = 0; i < pad; i++) c->send[c->send_used++] = 0;
}

/* Append raw bytes (must be 4-aligned in length) */
static void wl_push_bytes(wl_client_t *c, const void *data, uint32_t len) {
    if (c->send_used + (int)len > WL_SEND_BUF) return;
    memcpy(c->send + c->send_used, data, len);
    c->send_used += len;
}

/* Begin a message header, return offset of size field so we can fill it later */
static int wl_begin_msg(wl_client_t *c, uint32_t obj_id, uint16_t opcode) {
    int off = c->send_used;
    wl_push_u32(c, obj_id);
    wl_push_u32(c, (uint32_t)opcode | 0u);  /* size placeholder, filled by wl_end_msg */
    return off;
}

/* Fill in the message size field (in bytes, including header) */
static void wl_end_msg(wl_client_t *c, int hdr_off) {
    uint16_t total = (uint16_t)(c->send_used - hdr_off);
    uint16_t op;
    memcpy(&op, c->send + hdr_off + 4, 2);
    uint32_t hdr2 = ((uint32_t)total << 16) | op;
    memcpy(c->send + hdr_off + 4, &hdr2, 4);
}

static uint32_t next_serial(wl_client_t *c) { return ++c->serial; }

/* ── Object lookup / management ─────────────────────────────────────────── */

static wl_obj_t *wl_find_obj(wl_client_t *c, uint32_t id) {
    for (int i = 0; i < c->n_objs; i++)
        if (c->objs[i].id == id && c->objs[i].type != OBJ_NONE)
            return &c->objs[i];
    return NULL;
}

static wl_obj_t *wl_new_obj(wl_client_t *c, uint32_t id, obj_type_t type, void *data) {
    /* Reuse deleted slot if available */
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].type == OBJ_NONE) {
            c->objs[i] = (wl_obj_t){ type, id, data };
            return &c->objs[i];
        }
    }
    if (c->n_objs >= MAX_OBJS_PER_CLIENT) return NULL;
    c->objs[c->n_objs] = (wl_obj_t){ type, id, data };
    return &c->objs[c->n_objs++];
}

static void wl_delete_obj(wl_client_t *c, uint32_t id) {
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].id == id) {
            if (c->objs[i].type == OBJ_SURFACE) {
                wl_surface_t *s = c->objs[i].data;
                if (s) free(s);
            } else if (c->objs[i].type == OBJ_SHM_POOL || c->objs[i].type == OBJ_BUFFER) {
                wl_shm_buf_t *b = c->objs[i].data;
                if (b) {
                    if (b->data) munmap(b->data, b->size);
                    if (b->fd >= 0) close(b->fd);
                    free(b);
                }
            } else {
                free(c->objs[i].data);
            }
            c->objs[i].type = OBJ_NONE;
            c->objs[i].data = NULL;
            return;
        }
    }
}

/* ── Event senders ───────────────────────────────────────────────────────── */

static void send_wl_display_error(wl_client_t *c, uint32_t obj_id,
                                   uint32_t code, const char *msg) {
    int h = wl_begin_msg(c, 1, WL_DISPLAY_ERROR);
    wl_push_u32(c, obj_id);
    wl_push_u32(c, code);
    wl_push_str(c, msg);
    wl_end_msg(c, h);
}

static void send_wl_callback_done(wl_client_t *c, uint32_t cb_id, uint32_t serial) {
    int h = wl_begin_msg(c, cb_id, WL_CALLBACK_DONE);
    wl_push_u32(c, serial);
    wl_end_msg(c, h);
    /* callback objects are single-use — delete after firing */
    wl_delete_obj(c, cb_id);
    /* send wl_display::delete_id */
    int h2 = wl_begin_msg(c, 1, WL_DISPLAY_DELETE_ID);
    wl_push_u32(c, cb_id);
    wl_end_msg(c, h2);
}

static void send_registry_global(wl_client_t *c, uint32_t reg_id,
                                  uint32_t name, const char *iface, uint32_t ver) {
    int h = wl_begin_msg(c, reg_id, WL_REGISTRY_GLOBAL);
    wl_push_u32(c, name);
    wl_push_str(c, iface);
    wl_push_u32(c, ver);
    wl_end_msg(c, h);
}

/* Advertise all globals to the registry object */
static void advertise_globals(wl_client_t *c, uint32_t reg_id) {
    send_registry_global(c, reg_id,  1, "wl_compositor",          4);
    send_registry_global(c, reg_id,  2, "wl_shm",                 1);
    send_registry_global(c, reg_id,  3, "wl_seat",                7);
    send_registry_global(c, reg_id,  4, "wl_output",              4);
    send_registry_global(c, reg_id,  5, "xdg_wm_base",            3);
    send_registry_global(c, reg_id,  6, "wl_data_device_manager", 3);
}

static void send_shm_formats(wl_client_t *c, uint32_t shm_id) {
    int h;
    h = wl_begin_msg(c, shm_id, WL_SHM_FORMAT);
    wl_push_u32(c, WL_SHM_FORMAT_ARGB8888);
    wl_end_msg(c, h);
    h = wl_begin_msg(c, shm_id, WL_SHM_FORMAT);
    wl_push_u32(c, WL_SHM_FORMAT_XRGB8888);
    wl_end_msg(c, h);
}

static void send_seat_capabilities(wl_client_t *c, uint32_t seat_id) {
    int h = wl_begin_msg(c, seat_id, WL_SEAT_CAPABILITIES);
    wl_push_u32(c, WL_SEAT_CAP_KEYBOARD | WL_SEAT_CAP_POINTER);
    wl_end_msg(c, h);
    h = wl_begin_msg(c, seat_id, WL_SEAT_NAME);
    wl_push_str(c, "fifi-seat");
    wl_end_msg(c, h);
}

static void send_output_info(wl_client_t *c, uint32_t out_id) {
    /* geometry */
    int h = wl_begin_msg(c, out_id, WL_OUTPUT_GEOMETRY);
    wl_push_u32(c, 0);        /* x */
    wl_push_u32(c, 0);        /* y */
    wl_push_u32(c, (uint32_t)(g_w * 25 / 96));   /* phys width mm */
    wl_push_u32(c, (uint32_t)(g_h * 25 / 96));   /* phys height mm */
    wl_push_u32(c, 0);        /* subpixel: unknown */
    wl_push_str(c, "FiFi OS");
    wl_push_str(c, "Virtual");
    wl_push_u32(c, 0);        /* transform: normal */
    wl_end_msg(c, h);
    /* mode */
    h = wl_begin_msg(c, out_id, WL_OUTPUT_MODE);
    wl_push_u32(c, 3);        /* flags: current | preferred */
    wl_push_u32(c, (uint32_t)g_w);
    wl_push_u32(c, (uint32_t)g_h);
    wl_push_u32(c, 60000);    /* refresh: 60 Hz */
    wl_end_msg(c, h);
    /* scale */
    h = wl_begin_msg(c, out_id, WL_OUTPUT_SCALE);
    wl_push_u32(c, 1);
    wl_end_msg(c, h);
    /* done */
    h = wl_begin_msg(c, out_id, WL_OUTPUT_DONE);
    wl_end_msg(c, h);
}

static void send_xdg_surface_configure(wl_client_t *c, wl_surface_t *s) {
    uint32_t ser = next_serial(c);
    s->serial = ser;
    /* xdg_toplevel configure: states array (empty = normal window) */
    int h = wl_begin_msg(c, s->xdg_toplevel_id, XDG_TOPLEVEL_CONFIGURE);
    wl_push_u32(c, (uint32_t)s->w ? (uint32_t)s->w : (uint32_t)g_w);
    wl_push_u32(c, (uint32_t)s->h ? (uint32_t)s->h : (uint32_t)g_h);
    wl_push_u32(c, 0);  /* states array length = 0 */
    wl_end_msg(c, h);
    /* xdg_surface configure */
    h = wl_begin_msg(c, s->xdg_surface_id, XDG_SURFACE_CONFIGURE);
    wl_push_u32(c, ser);
    wl_end_msg(c, h);
}

/* ── Keymap ──────────────────────────────────────────────────────────────── */

/* Minimal XKB keymap string — covers basic ASCII + function keys.
 * Required by wl_keyboard.keymap to let clients process keyboard events. */
static const char s_keymap[] =
    "xkb_keymap {\n"
    "  xkb_keycodes { include \"evdev\" };\n"
    "  xkb_types    { include \"complete\" };\n"
    "  xkb_compat   { include \"complete\" };\n"
    "  xkb_symbols  { include \"pc+us+inet(evdev)\" };\n"
    "  xkb_geometry { include \"pc(pc105)\" };\n"
    "};\n";

static void send_keymap(wl_client_t *c) {
    if (!c->keyboard_id) return;
    /* Write keymap to a memfd so we can pass an fd to the client */
    int kfd = -1;
#ifdef __linux__
    kfd = (int)syscall(319 /*memfd_create*/, "xkb-keymap", 1 /*MFD_CLOEXEC*/);
#endif
    if (kfd < 0) {
        /* fallback: tmpfile */
        char tmp[] = "/tmp/fifi-keymap-XXXXXX";
        kfd = mkstemp(tmp);
        if (kfd >= 0) unlink(tmp);
    }
    if (kfd < 0) return;
    size_t klen = strlen(s_keymap) + 1;
    if (write(kfd, s_keymap, klen) != (ssize_t)klen) { close(kfd); return; }
    lseek(kfd, 0, SEEK_SET);

    /* Send keymap event — we need to pass an fd via ancillary data.
     * We use sendmsg() to send the socket fd alongside the message. */
    uint8_t buf[32];
    int hdr_off = 0;
    uint32_t obj  = c->keyboard_id;
    uint32_t hdr2 = ((uint32_t)24u << 16) | WL_KBD_KEYMAP;
    uint32_t fmt  = WL_KBD_KEYMAP_FORMAT_XKB;
    uint32_t sz   = (uint32_t)klen;
    memcpy(buf + 0,  &obj,  4);
    memcpy(buf + 4,  &hdr2, 4);
    memcpy(buf + 8,  &fmt,  4);
    memcpy(buf + 12, &sz,   4);
    (void)hdr_off;

    /* Build ancillary message with the fd */
    struct iovec iov = { buf, 16 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msgh = {0};
    msgh.msg_iov    = &iov;
    msgh.msg_iovlen = 1;
    msgh.msg_control    = cmsgbuf;
    msgh.msg_controllen = sizeof(cmsgbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msgh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &kfd, sizeof(int));
    sendmsg(c->fd, &msgh, MSG_NOSIGNAL);
    close(kfd);

    /* keyboard enter with empty keys pressed */
    int h = wl_begin_msg(c, c->keyboard_id, WL_KBD_ENTER);
    wl_push_u32(c, next_serial(c));
    wl_push_u32(c, 0);   /* surface id — 0 for now */
    wl_push_u32(c, 0);   /* keys array length */
    wl_end_msg(c, h);

    /* repeat info */
    h = wl_begin_msg(c, c->keyboard_id, WL_KBD_REPEAT_INFO);
    wl_push_u32(c, 25);   /* rate */
    wl_push_u32(c, 300);  /* delay ms */
    wl_end_msg(c, h);
}

/* ── Message dispatch ─────────────────────────────────────────────────────── */

static void wl_handle_msg(wl_client_t *c, uint32_t obj_id, uint16_t opcode,
                           const uint8_t *args, uint32_t args_len) {
    wl_obj_t *obj = wl_find_obj(c, obj_id);
    obj_type_t type = obj ? obj->type : (obj_id == 1 ? OBJ_DISPLAY : OBJ_NONE);

    switch (type) {

    /* ── wl_display ──────────────────────────────────────────────────── */
    case OBJ_DISPLAY:
        if (opcode == WL_DISPLAY_SYNC) {
            if (args_len < 4) break;
            uint32_t cb_id; memcpy(&cb_id, args, 4);
            wl_new_obj(c, cb_id, OBJ_CALLBACK, NULL);
            send_wl_callback_done(c, cb_id, g_global_serial++);
        } else if (opcode == WL_DISPLAY_GET_REGISTRY) {
            if (args_len < 4) break;
            uint32_t reg_id; memcpy(&reg_id, args, 4);
            wl_new_obj(c, reg_id, OBJ_REGISTRY, NULL);
            advertise_globals(c, reg_id);
        }
        break;

    /* ── wl_registry ─────────────────────────────────────────────────── */
    case OBJ_REGISTRY:
        if (opcode == WL_REGISTRY_BIND && args_len >= 16) {
            uint32_t name, iface_len, ver, new_id;
            memcpy(&name,    args,      4);
            memcpy(&iface_len, args+4,  4);
            /* iface string starts at args+8, padded to 4 bytes */
            const char *iface = (const char *)(args + 8);
            uint32_t padded = (iface_len + 3) & ~3u;
            if (8 + padded + 8 > args_len) break;
            memcpy(&ver,    args + 8 + padded,     4);
            memcpy(&new_id, args + 8 + padded + 4, 4);

            fprintf(stderr, "[wayland] bind name=%u iface=%.*s ver=%u id=%u\n",
                    name, (int)iface_len, iface, ver, new_id);

            if (name == 1 && strncmp(iface, "wl_compositor", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_COMPOSITOR, NULL);
                c->compositor_id = new_id;
            } else if (name == 2 && strncmp(iface, "wl_shm", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_SHM, NULL);
                c->shm_id = new_id;
                send_shm_formats(c, new_id);
            } else if (name == 3 && strncmp(iface, "wl_seat", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_SEAT, NULL);
                c->seat_id = new_id;
                send_seat_capabilities(c, new_id);
            } else if (name == 4 && strncmp(iface, "wl_output", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_OUTPUT, NULL);
                c->output_id = new_id;
                send_output_info(c, new_id);
            } else if (name == 5 && strncmp(iface, "xdg_wm_base", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_XDG_WM_BASE, NULL);
                c->xdg_wm_id = new_id;
                /* ping so client knows we're alive */
                int h = wl_begin_msg(c, new_id, XDG_WM_BASE_PING);
                wl_push_u32(c, g_global_serial++);
                wl_end_msg(c, h);
            } else if (name == 6 && strncmp(iface, "wl_data_device_manager", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_DATA_DEVICE_MGR, NULL);
            } else {
                send_wl_display_error(c, 1, 0, "unknown global");
            }
        }
        break;

    /* ── wl_compositor ───────────────────────────────────────────────── */
    case OBJ_COMPOSITOR:
        if (opcode == WL_COMPOSITOR_CREATE_SURFACE && args_len >= 4) {
            uint32_t sid; memcpy(&sid, args, 4);
            wl_surface_t *s = calloc(1, sizeof(wl_surface_t));
            wl_new_obj(c, sid, OBJ_SURFACE, s);
            fprintf(stderr, "[wayland] create surface id=%u\n", sid);
        } else if (opcode == WL_COMPOSITOR_CREATE_REGION && args_len >= 4) {
            uint32_t rid; memcpy(&rid, args, 4);
            wl_new_obj(c, rid, OBJ_REGION, NULL);
        }
        break;

    /* ── wl_surface ──────────────────────────────────────────────────── */
    case OBJ_SURFACE: {
        wl_surface_t *s = obj ? obj->data : NULL;
        if (!s) break;
        if (opcode == WL_SURFACE_ATTACH && args_len >= 12) {
            memcpy(&s->buffer_id, args, 4);
            /* dx, dy at args+4 and args+8 — ignore for now */
        } else if (opcode == WL_SURFACE_COMMIT) {
            /* Commit: if a buffer is attached, mark surface as mapped */
            if (s->buffer_id) {
                wl_obj_t *bobj = wl_find_obj(c, s->buffer_id);
                if (bobj && bobj->type == OBJ_BUFFER) {
                    s->buf = bobj->data;
                    s->mapped = true;
                    if (s->buf && s->buf->width && s->buf->height) {
                        s->w = s->buf->width;
                        s->h = s->buf->height;
                    }
                }
            }
            /* Send buffer_release so client can reuse the buffer */
            if (s->buffer_id) {
                int h = wl_begin_msg(c, s->buffer_id, WL_BUFFER_RELEASE);
                wl_end_msg(c, h);
            }
        } else if (opcode == WL_SURFACE_FRAME && args_len >= 4) {
            uint32_t cb_id; memcpy(&cb_id, args, 4);
            wl_new_obj(c, cb_id, OBJ_CALLBACK, NULL);
            /* Fire immediately — we don't do vsync scheduling yet */
            send_wl_callback_done(c, cb_id, g_global_serial++);
        } else if (opcode == WL_SURFACE_DESTROY) {
            s->mapped = false;
            wl_delete_obj(c, obj_id);
        }
        break;
    }

    /* ── wl_shm ──────────────────────────────────────────────────────── */
    case OBJ_SHM:
        if (opcode == WL_SHM_CREATE_POOL && args_len >= 12) {
            uint32_t pool_id; memcpy(&pool_id, args, 4);
            int32_t  sz;      memcpy(&sz,      args+8, 4);
            /* The fd arrives via cmsg ancillary data — we stash it in pool */
            wl_shm_buf_t *pool = calloc(1, sizeof(wl_shm_buf_t));
            pool->size = (size_t)sz;
            pool->fd   = -1;  /* fd set by caller after msg parse */
            wl_new_obj(c, pool_id, OBJ_SHM_POOL, pool);
        }
        break;

    /* ── wl_shm_pool ─────────────────────────────────────────────────── */
    case OBJ_SHM_POOL: {
        wl_shm_buf_t *pool = obj ? obj->data : NULL;
        if (!pool) break;
        if (opcode == WL_SHM_POOL_CREATE_BUFFER && args_len >= 24) {
            uint32_t buf_id;
            int32_t  offset, w, h, stride;
            uint32_t fmt;
            memcpy(&buf_id,  args,      4);
            memcpy(&offset,  args + 4,  4);
            memcpy(&w,       args + 8,  4);
            memcpy(&h,       args + 12, 4);
            memcpy(&stride,  args + 16, 4);
            memcpy(&fmt,     args + 20, 4);
            wl_shm_buf_t *buf = calloc(1, sizeof(wl_shm_buf_t));
            buf->fd     = -1;
            buf->width  = w;
            buf->height = h;
            buf->stride = stride;
            buf->format = fmt;
            /* Map the pool fd if we have it */
            if (pool->fd >= 0) {
                void *mapped = mmap(NULL, (size_t)(h * stride),
                                    PROT_READ, MAP_SHARED, pool->fd, offset);
                if (mapped != MAP_FAILED) {
                    buf->data = mapped;
                    buf->size = (size_t)(h * stride);
                }
            }
            wl_new_obj(c, buf_id, OBJ_BUFFER, buf);
        } else if (opcode == WL_SHM_POOL_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;
    }

    /* ── wl_buffer ───────────────────────────────────────────────────── */
    case OBJ_BUFFER:
        if (opcode == WL_BUFFER_DESTROY)
            wl_delete_obj(c, obj_id);
        break;

    /* ── wl_seat ─────────────────────────────────────────────────────── */
    case OBJ_SEAT:
        if (opcode == WL_SEAT_GET_KEYBOARD && args_len >= 4) {
            uint32_t kid; memcpy(&kid, args, 4);
            wl_new_obj(c, kid, OBJ_KEYBOARD, NULL);
            c->keyboard_id = kid;
            send_keymap(c);
        } else if (opcode == WL_SEAT_GET_POINTER && args_len >= 4) {
            uint32_t pid; memcpy(&pid, args, 4);
            wl_new_obj(c, pid, OBJ_POINTER, NULL);
            c->pointer_id = pid;
        }
        break;

    /* ── xdg_wm_base ─────────────────────────────────────────────────── */
    case OBJ_XDG_WM_BASE:
        if (opcode == XDG_WM_BASE_PONG) {
            /* client responded to our ping — good */
        } else if (opcode == XDG_WM_BASE_GET_XDG_SURFACE && args_len >= 8) {
            uint32_t xdg_surf_id, surf_id;
            memcpy(&xdg_surf_id, args,   4);
            memcpy(&surf_id,     args+4, 4);
            wl_new_obj(c, xdg_surf_id, OBJ_XDG_SURFACE, NULL);
            /* link surface to its xdg_surface */
            wl_obj_t *so = wl_find_obj(c, surf_id);
            if (so && so->type == OBJ_SURFACE && so->data) {
                wl_surface_t *s = so->data;
                s->xdg_surface_id = xdg_surf_id;
            }
        } else if (opcode == XDG_WM_BASE_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;

    /* ── xdg_surface ─────────────────────────────────────────────────── */
    case OBJ_XDG_SURFACE:
        if (opcode == XDG_SURFACE_GET_TOPLEVEL && args_len >= 4) {
            uint32_t tl_id; memcpy(&tl_id, args, 4);
            wl_new_obj(c, tl_id, OBJ_XDG_TOPLEVEL, NULL);
            /* Find which surface owns this xdg_surface */
            for (int i = 0; i < c->n_objs; i++) {
                if (c->objs[i].type == OBJ_SURFACE && c->objs[i].data) {
                    wl_surface_t *s = c->objs[i].data;
                    if (s->xdg_surface_id == obj_id) {
                        s->xdg_toplevel_id = tl_id;
                        /* Send initial configure */
                        send_xdg_surface_configure(c, s);
                        break;
                    }
                }
            }
        } else if (opcode == XDG_SURFACE_ACK_CONFIGURE) {
            /* client acknowledged configure — nothing to do yet */
        } else if (opcode == XDG_SURFACE_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;

    /* ── xdg_toplevel ────────────────────────────────────────────────── */
    case OBJ_XDG_TOPLEVEL:
        if (opcode == XDG_TOPLEVEL_SET_TITLE && args_len >= 4) {
            uint32_t slen; memcpy(&slen, args, 4);
            if (slen > 0 && slen < args_len) {
                /* Find surface with this toplevel */
                for (int i = 0; i < c->n_objs; i++) {
                    if (c->objs[i].type == OBJ_SURFACE && c->objs[i].data) {
                        wl_surface_t *s = c->objs[i].data;
                        if (s->xdg_toplevel_id == obj_id) {
                            uint32_t copy = slen < 127 ? slen : 127;
                            memcpy(s->title, args + 4, copy);
                            s->title[copy] = '\0';
                            fprintf(stderr, "[wayland] toplevel title: %s\n", s->title);
                            break;
                        }
                    }
                }
            }
        } else if (opcode == XDG_TOPLEVEL_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;

    /* ── data device manager ─────────────────────────────────────────── */
    case OBJ_DATA_DEVICE_MGR:
        /* Stub — enough to satisfy client bind without errors */
        break;

    default:
        /* Unknown object — send error */
        fprintf(stderr, "[wayland] unknown obj=%u op=%u\n", obj_id, opcode);
        break;
    }
}

/* ── Receive messages from a client ──────────────────────────────────────── */

/* Receive any ancillary fds from a recvmsg — returns the first fd or -1 */
static int recv_fd(struct msghdr *msgh) {
    struct cmsghdr *cm = CMSG_FIRSTHDR(msgh);
    if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(cm), sizeof(int));
    return fd;
}

static void wl_client_recv(wl_client_t *c) {
    uint8_t     anc[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = { c->recv + c->recv_used, WL_RECV_BUF - c->recv_used };
    struct msghdr msgh = {0};
    msgh.msg_iov        = &iov;
    msgh.msg_iovlen     = 1;
    msgh.msg_control    = anc;
    msgh.msg_controllen = sizeof(anc);

    ssize_t n = recvmsg(c->fd, &msgh, MSG_DONTWAIT);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            fprintf(stderr, "[wayland] client fd=%d disconnected\n", c->fd);
            close(c->fd);
            c->fd     = -1;
            c->active = false;
        }
        return;
    }
    c->recv_used += (int)n;

    /* Extract any ancillary fd */
    int rx_fd = recv_fd(&msgh);

    /* Parse complete messages from recv buffer */
    while (c->recv_used >= 8) {
        uint32_t obj_id;
        uint32_t hdr2;
        memcpy(&obj_id, c->recv,     4);
        memcpy(&hdr2,   c->recv + 4, 4);
        uint16_t opcode = (uint16_t)(hdr2 & 0xFFFF);
        uint16_t msg_sz = (uint16_t)(hdr2 >> 16);
        if (msg_sz < 8) { /* malformed */ break; }
        if (c->recv_used < msg_sz) break;  /* incomplete */

        const uint8_t *args = c->recv + 8;
        uint32_t args_len   = msg_sz - 8;

        /* If this message is CREATE_POOL (wl_shm op 0), the fd arrives via cmsg */
        if (rx_fd >= 0) {
            /* Find the SHM pool that was just created — stash the fd */
            /* It's the most recently created OBJ_SHM_POOL without a fd */
            for (int i = c->n_objs - 1; i >= 0; i--) {
                if (c->objs[i].type == OBJ_SHM_POOL && c->objs[i].data) {
                    wl_shm_buf_t *pool = c->objs[i].data;
                    if (pool->fd < 0) {
                        pool->fd = rx_fd;
                        pool->data = mmap(NULL, pool->size, PROT_READ, MAP_SHARED, rx_fd, 0);
                        if (pool->data == MAP_FAILED) pool->data = NULL;
                        rx_fd = -1;
                        break;
                    }
                }
            }
            if (rx_fd >= 0) { close(rx_fd); rx_fd = -1; }
        }

        wl_handle_msg(c, obj_id, opcode, args, args_len);

        /* Consume message from buffer */
        memmove(c->recv, c->recv + msg_sz, c->recv_used - msg_sz);
        c->recv_used -= msg_sz;
    }
}

/* Flush send buffer to client socket */
static void wl_client_flush(wl_client_t *c) {
    if (c->send_used <= 0 || c->fd < 0) return;
    ssize_t n = send(c->fd, c->send, c->send_used, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
        memmove(c->send, c->send + n, c->send_used - n);
        c->send_used -= n;
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "[wayland] flush error fd=%d: %s\n", c->fd, strerror(errno));
        close(c->fd); c->fd = -1; c->active = false;
    }
}

/* ── Focus tracking ──────────────────────────────────────────────────────── */

static int      g_focus_ci  = -1;  /* client index with keyboard focus */
static uint32_t g_focus_sid = 0;   /* surface obj id with keyboard focus */
static int32_t  g_prev_mx = -1, g_prev_my = -1;
static uint8_t  g_prev_btns = 0;

/* Find the topmost mapped Wayland surface that contains (mx, my) */
static bool wl_surface_hit(wl_surface_t *s, int32_t mx, int32_t my) {
    if (!s || !s->mapped) return false;
    return mx >= s->x && mx < s->x + s->w &&
           my >= s->y && my < s->y + s->h;
}

/* Wl-fixed is 24.8 fixed-point (value * 256) */
static uint32_t wl_fixed(int32_t v) { return (uint32_t)(v * 256); }

/* Send pointer enter/leave events when focus changes */
static void wl_send_ptr_enter(wl_client_t *c, uint32_t surf_id,
                               int32_t mx, int32_t my) {
    if (!c->pointer_id) return;
    uint32_t ser = next_serial(c);
    int h = wl_begin_msg(c, c->pointer_id, WL_PTR_ENTER);
    wl_push_u32(c, ser);
    wl_push_u32(c, surf_id);
    wl_push_u32(c, wl_fixed(mx));
    wl_push_u32(c, wl_fixed(my));
    wl_end_msg(c, h);
    h = wl_begin_msg(c, c->pointer_id, WL_PTR_FRAME);
    wl_end_msg(c, h);
}

static void wl_send_ptr_leave(wl_client_t *c, uint32_t surf_id) {
    if (!c->pointer_id) return;
    uint32_t ser = next_serial(c);
    int h = wl_begin_msg(c, c->pointer_id, WL_PTR_LEAVE);
    wl_push_u32(c, ser);
    wl_push_u32(c, surf_id);
    wl_end_msg(c, h);
    h = wl_begin_msg(c, c->pointer_id, WL_PTR_FRAME);
    wl_end_msg(c, h);
}

static void wl_send_kbd_enter(wl_client_t *c, uint32_t surf_id) {
    if (!c->keyboard_id) return;
    uint32_t ser = next_serial(c);
    int h = wl_begin_msg(c, c->keyboard_id, WL_KBD_ENTER);
    wl_push_u32(c, ser);
    wl_push_u32(c, surf_id);
    wl_push_u32(c, 0);  /* empty keys array */
    wl_end_msg(c, h);
}

static void wl_send_kbd_leave(wl_client_t *c, uint32_t surf_id) {
    if (!c->keyboard_id) return;
    uint32_t ser = next_serial(c);
    int h = wl_begin_msg(c, c->keyboard_id, WL_KBD_LEAVE);
    wl_push_u32(c, ser);
    wl_push_u32(c, surf_id);
    wl_end_msg(c, h);
}

/* Deliver mouse events to Wayland surfaces.
 * Call from main.c after input_poll() with current mouse state. */
void wayland_send_mouse(int32_t mx, int32_t my, uint8_t btns) {
    /* Find topmost surface under cursor */
    int  new_ci  = -1;
    uint32_t new_sid = 0;
    wl_surface_t *new_s = NULL;

    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        wl_client_t *c = &g_wl_clients[ci];
        if (!c->active) continue;
        for (int oi = 0; oi < c->n_objs; oi++) {
            if (c->objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = c->objs[oi].data;
            if (wl_surface_hit(s, mx, my)) {
                new_ci  = ci;
                new_sid = c->objs[oi].id;
                new_s   = s;
                /* keep searching — last (topmost draw order) wins */
            }
        }
    }

    /* Handle focus changes */
    if (new_ci != g_focus_ci || new_sid != g_focus_sid) {
        /* Leave old surface */
        if (g_focus_ci >= 0 && g_focus_sid) {
            wl_client_t *oc = &g_wl_clients[g_focus_ci];
            if (oc->active) {
                wl_send_ptr_leave(oc, g_focus_sid);
                wl_send_kbd_leave(oc, g_focus_sid);
                wl_client_flush(oc);
            }
        }
        /* Enter new surface */
        if (new_ci >= 0 && new_sid) {
            wl_client_t *nc = &g_wl_clients[new_ci];
            wl_surface_t *s_loc = new_s;
            wl_send_ptr_enter(nc, new_sid, mx - s_loc->x, my - s_loc->y);
            wl_send_kbd_enter(nc, new_sid);
            wl_client_flush(nc);
        }
        g_focus_ci  = new_ci;
        g_focus_sid = new_sid;
    }

    if (g_focus_ci < 0 || !g_focus_sid) { g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns; return; }

    wl_client_t *fc = &g_wl_clients[g_focus_ci];
    if (!fc->active || !fc->pointer_id) { g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns; return; }

    /* Find focused surface to compute local coords */
    wl_surface_t *fs = NULL;
    for (int oi = 0; oi < fc->n_objs; oi++) {
        if (fc->objs[oi].id == g_focus_sid && fc->objs[oi].type == OBJ_SURFACE)
            fs = fc->objs[oi].data;
    }
    int32_t lx = fs ? mx - fs->x : mx;
    int32_t ly = fs ? my - fs->y : my;

    /* Motion */
    if (mx != g_prev_mx || my != g_prev_my) {
        int h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_MOTION);
        wl_push_u32(fc, (uint32_t)(time(NULL) * 1000));  /* time ms */
        wl_push_u32(fc, wl_fixed(lx));
        wl_push_u32(fc, wl_fixed(ly));
        wl_end_msg(fc, h);
    }

    /* Button changes — Wayland uses Linux BTN codes directly */
    uint8_t changed = btns ^ g_prev_btns;
    if (changed) {
        /* BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112 */
        static const uint32_t btn_codes[3] = { 0x110, 0x111, 0x112 };
        for (int b = 0; b < 3; b++) {
            if (!(changed & (1u << b))) continue;
            uint32_t state = (btns >> b) & 1;  /* 1=pressed, 0=released */
            uint32_t ser = next_serial(fc);
            int h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_BUTTON);
            wl_push_u32(fc, ser);
            wl_push_u32(fc, (uint32_t)(time(NULL) * 1000));
            wl_push_u32(fc, btn_codes[b]);
            wl_push_u32(fc, state);
            wl_end_msg(fc, h);
        }
    }

    /* Frame event after motion/buttons */
    if (mx != g_prev_mx || my != g_prev_my || changed) {
        int h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_FRAME);
        wl_end_msg(fc, h);
        wl_client_flush(fc);
    }

    g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns;
}

/* Deliver a key event to the focused Wayland surface.
 * key is a Linux evdev keycode (KEY_A=30, etc.), state: 1=press, 0=release. */
void wayland_send_key(uint32_t evdev_key, uint32_t state) {
    if (g_focus_ci < 0 || !g_focus_sid) return;
    wl_client_t *fc = &g_wl_clients[g_focus_ci];
    if (!fc->active || !fc->keyboard_id) return;
    uint32_t ser = next_serial(fc);
    int h = wl_begin_msg(fc, fc->keyboard_id, WL_KBD_KEY);
    wl_push_u32(fc, ser);
    wl_push_u32(fc, (uint32_t)(time(NULL) * 1000));
    wl_push_u32(fc, evdev_key);
    wl_push_u32(fc, state);
    wl_end_msg(fc, h);
    /* send modifiers (all zero for now) */
    h = wl_begin_msg(fc, fc->keyboard_id, WL_KBD_MODIFIERS);
    wl_push_u32(fc, next_serial(fc));
    wl_push_u32(fc, 0); /* depressed */
    wl_push_u32(fc, 0); /* latched */
    wl_push_u32(fc, 0); /* locked */
    wl_push_u32(fc, 0); /* group */
    wl_end_msg(fc, h);
    wl_client_flush(fc);
}

/* Returns true if a Wayland surface has keyboard focus */
bool wayland_has_focus(void) { return g_focus_ci >= 0; }

/* ── Blit Wayland surfaces to the FiFi framebuffer ───────────────────────── */

/* Called from compositor main after ipc_blit_all() */
void wayland_blit_surfaces(void) {
    extern void console_paste_rect(const uint32_t *src, uint64_t dx, uint64_t dy,
                                    uint64_t w, uint64_t h);
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        wl_client_t *c = &g_wl_clients[ci];
        if (!c->active) continue;
        for (int oi = 0; oi < c->n_objs; oi++) {
            if (c->objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = c->objs[oi].data;
            if (!s || !s->mapped || !s->buf || !s->buf->data) continue;
            if (s->w <= 0 || s->h <= 0) continue;
            console_paste_rect((const uint32_t *)s->buf->data,
                               (uint64_t)s->x, (uint64_t)s->y,
                               (uint64_t)s->w, (uint64_t)s->h);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Set display resolution — call once after framebuffer is known */
void wayland_set_display_size(int w, int h) {
    g_w = w; g_h = h;
}

/* Return the listening socket fd for inclusion in poll() */
int wayland_server_fd(void) { return g_wl_fd; }

/* Initialize: create Unix socket at $XDG_RUNTIME_DIR/wayland-0 */
bool wayland_init(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg || !xdg[0]) xdg = "/tmp";
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/wayland-0", xdg);

    unlink(g_sock_path);  /* remove stale socket */

    g_wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_wl_fd < 0) {
        fprintf(stderr, "[wayland] socket() failed: %s\n", strerror(errno));
        return false;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_wl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[wayland] bind %s failed: %s\n", g_sock_path, strerror(errno));
        close(g_wl_fd); g_wl_fd = -1; return false;
    }
    chmod(g_sock_path, 0700);

    if (listen(g_wl_fd, 8) < 0) {
        fprintf(stderr, "[wayland] listen failed: %s\n", strerror(errno));
        close(g_wl_fd); g_wl_fd = -1; return false;
    }

    /* Set WAYLAND_DISPLAY for child processes */
    setenv("WAYLAND_DISPLAY", "wayland-0", 1);
    if (xdg) setenv("XDG_RUNTIME_DIR", xdg, 1);

    fprintf(stderr, "[wayland] listening on %s\n", g_sock_path);
    memset(g_wl_clients, 0, sizeof(g_wl_clients));
    for (int i = 0; i < MAX_WL_CLIENTS; i++) g_wl_clients[i].fd = -1;
    return true;
}

/* Poll: accept new connections, read/dispatch messages, flush sends */
void wayland_poll(void) {
    if (g_wl_fd < 0) return;

    /* Accept new clients */
    for (;;) {
        int cfd = accept4(g_wl_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) break;
        bool accepted = false;
        for (int i = 0; i < MAX_WL_CLIENTS; i++) {
            if (!g_wl_clients[i].active) {
                memset(&g_wl_clients[i], 0, sizeof(wl_client_t));
                g_wl_clients[i].fd     = cfd;
                g_wl_clients[i].active = true;
                g_wl_clients[i].serial = 1;
                fprintf(stderr, "[wayland] new client fd=%d slot=%d\n", cfd, i);
                accepted = true;
                break;
            }
        }
        if (!accepted) { close(cfd); }
    }

    /* Poll each active client */
    for (int i = 0; i < MAX_WL_CLIENTS; i++) {
        wl_client_t *c = &g_wl_clients[i];
        if (!c->active || c->fd < 0) continue;
        wl_client_recv(c);
        wl_client_flush(c);
    }
}

void wayland_shutdown(void) {
    for (int i = 0; i < MAX_WL_CLIENTS; i++) {
        if (g_wl_clients[i].fd >= 0) {
            close(g_wl_clients[i].fd);
            g_wl_clients[i].fd = -1;
        }
    }
    if (g_wl_fd >= 0) { close(g_wl_fd); g_wl_fd = -1; }
    if (g_sock_path[0]) unlink(g_sock_path);
}
