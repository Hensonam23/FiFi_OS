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

/* wl_data_device_manager opcodes (requests) */
#define WL_DDM_CREATE_DATA_SOURCE 0
#define WL_DDM_GET_DATA_DEVICE    1

/* wl_data_device opcodes */
#define WL_DD_START_DRAG          0   /* request */
#define WL_DD_SET_SELECTION       1   /* request */
#define WL_DD_RELEASE             2   /* request */
#define WL_DD_DATA_OFFER          0   /* event */
#define WL_DD_SELECTION           3   /* event */

/* wl_data_source opcodes (requests) */
#define WL_DS_OFFER               0
#define WL_DS_DESTROY             1
#define WL_DS_SET_ACTIONS         2

/* wl_subcompositor opcodes (requests) */
#define WL_SUBCOMP_DESTROY        0
#define WL_SUBCOMP_GET_SUBSURFACE 1

/* wl_subsurface opcodes (requests) */
#define WL_SUBSURF_DESTROY        0
#define WL_SUBSURF_SET_POSITION   1
#define WL_SUBSURF_PLACE_ABOVE    2
#define WL_SUBSURF_PLACE_BELOW    3
#define WL_SUBSURF_SET_SYNC       4
#define WL_SUBSURF_SET_DESYNC     5

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
    OBJ_SUBCOMPOSITOR,
    OBJ_SUBSURFACE,
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
    /* wl_subsurface role: position is relative to parent_surface_id. Firefox/GTK
     * REQUIRE wl_subcompositor (MOZ_RELEASE_ASSERT(GetSubcompositor())) and render
     * web content into a subsurface of the toplevel — without this they crash. */
    bool         is_subsurface;
    uint32_t     parent_surface_id;
    int32_t      sub_x, sub_y;
    /* Pending frame callback: fired during commit (not on frame request).
     * Firing immediately on frame request causes clients to destroy buffers
     * before we process the commit, breaking the render pipeline. */
    uint32_t     pending_frame_cb;
} wl_surface_t;

/* ── Client ───────────────────────────────────────────────────────────────── */
#define WL_RECV_BUF 65536
#define WL_SEND_BUF 65536
#define MAX_OBJS_PER_CLIENT 512

struct wl_client {
    int       fd;
    bool      active;
    bool      send_overflow;   /* true if a push overflowed — discard message */
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

/* ── Orphan buffer pool ────────────────────────────────────────────────────── */
/* Firefox/LibreWolf creates buffers in a GPU-process connection that disconnects
 * before the parent process commits surfaces. Buffers are saved here so they
 * survive the disconnect and the cross-client commit lookup can find them. */
#define MAX_ORPHAN_BUFS 64
typedef struct { uint32_t id; wl_shm_buf_t *buf; } orphan_buf_t;
static orphan_buf_t g_orphan_bufs[MAX_ORPHAN_BUFS];
static int          g_n_orphans = 0;

static void orphan_save_buffers(wl_client_t *c) {
    int saved = 0;
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].type != OBJ_BUFFER || !c->objs[i].data) continue;
        if (g_n_orphans >= MAX_ORPHAN_BUFS) break;
        wl_shm_buf_t *b = c->objs[i].data;
        fprintf(stderr, "[orphan] saving fd=%d buf_id=%u w=%d h=%d data=%p\n",
                c->fd, c->objs[i].id, b->width, b->height, b->data);
        g_orphan_bufs[g_n_orphans].id  = c->objs[i].id;
        g_orphan_bufs[g_n_orphans].buf = b;
        g_n_orphans++;
        saved++;
        c->objs[i].data = NULL;  /* prevent free in slot-clear */
    }
    if (saved) fprintf(stderr, "[orphan] saved %d buffers from fd=%d total=%d\n", saved, c->fd, g_n_orphans);
}

static wl_shm_buf_t *orphan_find(uint32_t id) {
    for (int i = 0; i < g_n_orphans; i++)
        if (g_orphan_bufs[i].id == id) return g_orphan_bufs[i].buf;
    return NULL;
}

/* wl_find_obj_any is defined after the server state globals below */

/* ── Server state ─────────────────────────────────────────────────────────── */
#define MAX_WL_CLIENTS 16
static int         g_wl_fd      = -1;   /* listening socket */
static wl_client_t g_wl_clients[MAX_WL_CLIENTS];
static uint32_t    g_global_serial = 1;
static int         g_w = 1024, g_h = 768;  /* display size — set from framebuffer */
static char        g_sock_path[128] = {0};

/* ── Wire protocol helpers ───────────────────────────────────────────────── */

/* Append a 32-bit word to the send buffer */
static void wl_push_u32(wl_client_t *c, uint32_t v) {
    if (c->send_used + 4 > WL_SEND_BUF) { c->send_overflow = true; return; }
    memcpy(c->send + c->send_used, &v, 4);
    c->send_used += 4;
}

/* Append a Wayland string (length-prefixed, NUL-terminated, 4-aligned) */
static void wl_push_str(wl_client_t *c, const char *s) {
    uint32_t slen = s ? (uint32_t)strlen(s) + 1 : 0;
    uint32_t pad  = (4 - (slen % 4)) % 4;
    wl_push_u32(c, slen);
    if (c->send_used + (int)(slen + pad) > WL_SEND_BUF) { c->send_overflow = true; return; }
    if (slen) { memcpy(c->send + c->send_used, s, slen); c->send_used += slen; }
    for (uint32_t i = 0; i < pad; i++) c->send[c->send_used++] = 0;
}

/* Append raw bytes (must be 4-aligned in length) */
static void wl_push_bytes(wl_client_t *c, const void *data, uint32_t len) {
    if (c->send_used + (int)len > WL_SEND_BUF) { c->send_overflow = true; return; }
    memcpy(c->send + c->send_used, data, len);
    c->send_used += len;
}

/* Begin a message header, return offset of size field so we can fill it later */
static int wl_begin_msg(wl_client_t *c, uint32_t obj_id, uint16_t opcode) {
    int off = c->send_used;
    c->send_overflow = false;   /* reset per-message overflow flag */
    wl_push_u32(c, obj_id);
    wl_push_u32(c, (uint32_t)opcode | 0u);  /* size placeholder, filled by wl_end_msg */
    return off;
}

/* Fill in the message size field — rolls back the message if an overflow occurred */
static void wl_end_msg(wl_client_t *c, int hdr_off) {
    if (c->send_overflow) {
        /* Discard the partial message rather than sending corrupt data */
        fprintf(stderr, "[wayland] send buffer overflow — dropping message\n");
        c->send_used = hdr_off;
        c->send_overflow = false;
        return;
    }
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
    /* If this ID already exists (from a previous session or re-use), overwrite it */
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].id == id) {
            /* Free old data if it was a surface or buffer */
            if (c->objs[i].type == OBJ_SURFACE && c->objs[i].data) free(c->objs[i].data);
            c->objs[i] = (wl_obj_t){ type, id, data };
            return &c->objs[i];
        }
    }
    /* Reuse a deleted (OBJ_NONE) slot */
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

/* Forward declaration — defined after server state globals */
static int pending_fd_pop(void);

/* Find an object in ANY client slot (active or zombie) */
static wl_obj_t *wl_find_obj_any(uint32_t id) {
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        if (g_wl_clients[ci].n_objs == 0) continue;
        wl_obj_t *o = wl_find_obj(&g_wl_clients[ci], id);
        if (o) return o;
    }
    return NULL;
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
    send_registry_global(c, reg_id,  7, "wl_subcompositor",       1);
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

static void wl_client_flush(wl_client_t *c);   /* defined below */

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
    /* Message size is header(8)+format(4)+size(4)=16. The fd travels out-of-band
     * via SCM_RIGHTS and must NOT be counted here — declaring 24 desyncs the
     * client stream ("message too short, invalid header"). */
    uint32_t hdr2 = ((uint32_t)16u << 16) | WL_KBD_KEYMAP;
    uint32_t fmt  = WL_KBD_KEYMAP_FORMAT_XKB;
    uint32_t sz   = (uint32_t)klen;
    memcpy(buf + 0,  &obj,  4);
    memcpy(buf + 4,  &hdr2, 4);
    memcpy(buf + 8,  &fmt,  4);
    memcpy(buf + 12, &sz,   4);
    (void)hdr_off;

    /* Drain any buffered events FIRST. The keymap goes out via a raw sendmsg()
     * (below) to carry the fd, bypassing the c->send buffer; if buffered events
     * are still pending they would arrive AFTER the keymap, corrupting message
     * order (clients then see bogus objects/opcodes and stall during init). */
    wl_client_flush(c);

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

    /* NOTE: do NOT send wl_keyboard.enter here. enter's surface argument is
     * non-nullable; sending it with surface=0 before any surface has focus makes
     * libwayland abort ("NULL object received on non-nullable type, enter(uoa)").
     * The real enter is sent from wl_send_kbd_enter() on actual focus change. */

    /* repeat info */
    int h = wl_begin_msg(c, c->keyboard_id, WL_KBD_REPEAT_INFO);
    wl_push_u32(c, 25);   /* rate */
    wl_push_u32(c, 300);  /* delay ms */
    wl_end_msg(c, h);
}

/* ── Message dispatch ─────────────────────────────────────────────────────── */

static void wl_handle_msg(wl_client_t *c, uint32_t obj_id, uint16_t opcode,
                           const uint8_t *args, uint32_t args_len) {
    wl_obj_t *obj = wl_find_obj(c, obj_id);
    /* Firefox reconnects and sends ops for objects from previous connections.
     * If not found in current client, search all other slots (active or zombie). */
    wl_client_t *obj_owner = c;
    if (!obj && obj_id != 1) {
        for (int _zi = 0; _zi < MAX_WL_CLIENTS; _zi++) {
            if (&g_wl_clients[_zi] == c || g_wl_clients[_zi].n_objs == 0) continue;
            wl_obj_t *_zo = wl_find_obj(&g_wl_clients[_zi], obj_id);
            if (_zo) { obj = _zo; obj_owner = &g_wl_clients[_zi]; break; }
        }
    }
    obj_type_t type = obj ? obj->type : (obj_id == 1 ? OBJ_DISPLAY : OBJ_NONE);

    /* Per-request trace (off by default; set FIFI_WL_TRACE=1 to enable). Logs every
     * dispatched request so a client crash can be located from the SERVER's last
     * processed message — pairs with the "unknown obj" line below for unhandled ops.
     * Used to debug the LibreWolf startup crash; harmless when the env is unset. */
    static int s_trace = -1;
    if (s_trace < 0) s_trace = getenv("FIFI_WL_TRACE") ? 1 : 0;
    if (s_trace)
        fprintf(stderr, "[wl-trace] fd=%d obj=%u type=%d op=%u len=%u\n",
                c->fd, obj_id, (int)type, opcode, args_len);

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
            } else if (name == 7 && strncmp(iface, "wl_subcompositor", iface_len) == 0) {
                /* Required by GTK/Firefox. Binding it makes Gecko's
                 * GetSubcompositor() non-null so its release-assert passes. */
                wl_new_obj(c, new_id, OBJ_SUBCOMPOSITOR, NULL);
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
            fprintf(stderr, "[region] create rid=%u fd=%d n_objs=%d\n", rid, c->fd, c->n_objs);
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
            /* Commit: if a buffer is attached, mark surface as mapped.
             * Multi-process clients (Firefox/LibreWolf) create buffers in one
             * client slot and surfaces in another. If the buffer isn't found in
             * the current client, search all other active clients. */
            if (s->buffer_id) {
                wl_obj_t *bobj = wl_find_obj_any(s->buffer_id);
                if (!bobj || bobj->type != OBJ_BUFFER) {
                    /* Cross-client buffer lookup for multi-process apps */
                    for (int _ci = 0; _ci < MAX_WL_CLIENTS; _ci++) {
                        if (!g_wl_clients[_ci].active || &g_wl_clients[_ci] == c) continue;
                        wl_obj_t *_b = wl_find_obj(&g_wl_clients[_ci], s->buffer_id);
                        if (_b && _b->type == OBJ_BUFFER) { bobj = _b; break; }
                    }
                }
                /* Also check orphan pool (buffers from disconnected clients) */
                wl_shm_buf_t *orphan_b = (!bobj || bobj->type != OBJ_BUFFER)
                    ? orphan_find(s->buffer_id) : NULL;
                if (bobj && bobj->type == OBJ_BUFFER) {
                    s->buf    = bobj->data;
                    s->mapped = true;
                    if (s->buf && s->buf->width && s->buf->height) {
                        s->w = s->buf->width; s->h = s->buf->height;
                    }
                } else if (orphan_b) {
                    s->buf    = orphan_b;
                    s->mapped = true;
                    if (s->buf && s->buf->width && s->buf->height) {
                        s->w = s->buf->width; s->h = s->buf->height;
                    }
                    fprintf(stderr, "[orphan] MAPPED obj=%u w=%d h=%d buf_id=%u data=%p\n",
                            obj_id, s->w, s->h, s->buffer_id, s->buf ? s->buf->data : NULL);
                } else {
                    fprintf(stderr, "[nomatch] obj=%u buf_id=%u not found anywhere\n",
                            obj_id, s->buffer_id);
                }
            }
            /* Fire pending frame callback (stored at WL_SURFACE_FRAME time) */
            if (s->pending_frame_cb) {
                send_wl_callback_done(c, s->pending_frame_cb, g_global_serial++);
                s->pending_frame_cb = 0;
            }
            /* Send buffer_release to the client that owns the buffer */
            if (s->buffer_id) {
                wl_client_t *buf_owner = c;
                /* If buffer was found cross-client, send release to its owner */
                for (int _ci = 0; _ci < MAX_WL_CLIENTS; _ci++) {
                    if (!g_wl_clients[_ci].active) continue;
                    if (wl_find_obj(&g_wl_clients[_ci], s->buffer_id)) {
                        buf_owner = &g_wl_clients[_ci]; break;
                    }
                }
                int h = wl_begin_msg(buf_owner, s->buffer_id, WL_BUFFER_RELEASE);
                wl_end_msg(buf_owner, h);
            }
        } else if (opcode == WL_SURFACE_FRAME && args_len >= 4) {
            uint32_t cb_id; memcpy(&cb_id, args, 4);
            wl_new_obj(c, cb_id, OBJ_CALLBACK, NULL);
            /* Store callback — fire it during commit so the client doesn't
             * free buffers before we've had a chance to process the commit. */
            if (s) s->pending_frame_cb = cb_id;
        } else if (opcode == WL_SURFACE_DESTROY) {
            s->mapped = false;
            wl_delete_obj(c, obj_id);
        }
        break;
    }

    /* ── wl_shm ──────────────────────────────────────────────────────── */
    case OBJ_SHM:
        /* create_pool: new_id(4) + fd(0 wire bytes, cmsg only) + size(4) = 8 bytes */
        if (opcode == WL_SHM_CREATE_POOL && args_len >= 8) {
            uint32_t pool_id; memcpy(&pool_id, args,   4);
            int32_t  sz;      memcpy(&sz,      args+4, 4);
            wl_shm_buf_t *pool = calloc(1, sizeof(wl_shm_buf_t));
            pool->size = (size_t)sz;
            pool->fd   = -1;
            /* Pop the next queued fd — each create_pool has exactly one fd via cmsg */
            int rx_fd = pending_fd_pop();
            if (rx_fd >= 0) {
                pool->fd   = rx_fd;
                pool->data = mmap(NULL, pool->size, PROT_READ, MAP_SHARED, rx_fd, 0);
                if (pool->data == MAP_FAILED) pool->data = NULL;
            }
            wl_new_obj(c, pool_id, OBJ_SHM_POOL, pool);
            fprintf(stderr, "[pool] create fd=%d pool_id=%u sz=%d pool_fd=%d data=%p\n",
                    c->fd, pool_id, sz, pool->fd, pool->data);
        }
        break;

    /* ── wl_shm_pool ─────────────────────────────────────────────────── */
    case OBJ_SHM_POOL: {
        wl_shm_buf_t *pool = obj ? obj->data : NULL;
        fprintf(stderr, "[pool] fd=%d op=%u pool_id=%u obj=%p pool=%p pool_fd=%d\n", c->fd, opcode, obj_id, (void*)obj, (void*)pool, pool ? pool->fd : -99);
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
            fprintf(stderr, "[pool] create_buffer buf_id=%u pool_fd=%d w=%d h=%d data=%p\n",
                    buf_id, pool->fd, w, h, buf->data);
            wl_new_obj(c, buf_id, OBJ_BUFFER, buf);
        } else if (opcode == WL_SHM_POOL_RESIZE && args_len >= 4) {
            /* Client grew the pool (it can only grow). Re-map at the new size so
             * pool->data stays valid; ignoring this left a stale undersized map
             * (e.g. 2304B while the client used 25216B) — real toolkits (GTK/Gecko)
             * resize the pool several times during init before creating buffers. */
            int32_t newsz; memcpy(&newsz, args, 4);
            if (newsz > 0 && (size_t)newsz > pool->size) {
                if (pool->fd >= 0) {
                    void *nm = mmap(NULL, (size_t)newsz, PROT_READ, MAP_SHARED, pool->fd, 0);
                    if (nm != MAP_FAILED) {
                        if (pool->data) munmap(pool->data, pool->size);
                        pool->data = nm;
                    }
                }
                pool->size = (size_t)newsz;
            }
        } else if (opcode == WL_SHM_POOL_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;
    }

    /* ── wl_buffer ───────────────────────────────────────────────────── */
    case OBJ_BUFFER:
        if (opcode == WL_BUFFER_DESTROY) {
            /* Null out any surface still holding a pointer to this buffer.
             * We send wl_buffer.release immediately on commit, so the client
             * is entitled to destroy it at any time — but s->buf may still
             * point here until the next commit. Clear to avoid use-after-free. */
            void *dying_buf = obj ? obj->data : NULL;
            if (dying_buf) {
                for (int _i = 0; _i < c->n_objs; _i++) {
                    if (c->objs[_i].type == OBJ_SURFACE && c->objs[_i].data) {
                        wl_surface_t *_s = c->objs[_i].data;
                        if (_s->buf == dying_buf) {
                            _s->buf    = NULL;
                            _s->mapped = false;
                        }
                    }
                }
            }
            wl_delete_obj(c, obj_id);
        }
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
            wl_obj_t *so = wl_find_obj_any(surf_id);
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
        /* The client creates new_id objects here; we MUST register them or the
         * IDs become phantoms (later requests on them hit the "unknown obj"
         * path). GTK/Gecko always call get_data_device during seat init. */
        if (opcode == WL_DDM_GET_DATA_DEVICE && args_len >= 4) {
            uint32_t dd_id; memcpy(&dd_id, args, 4);  /* arg1=new id, arg2=seat */
            wl_new_obj(c, dd_id, OBJ_DATA_DEVICE, NULL);
            /* No selection/clipboard yet: per protocol we simply send nothing
             * (an absent selection event means "empty"). */
        } else if (opcode == WL_DDM_CREATE_DATA_SOURCE && args_len >= 4) {
            uint32_t ds_id; memcpy(&ds_id, args, 4);
            wl_new_obj(c, ds_id, OBJ_DATA_SOURCE, NULL);
        }
        break;

    /* ── data device ─────────────────────────────────────────────────── */
    case OBJ_DATA_DEVICE:
        /* set_selection / start_drag: we don't implement clipboard transfer yet,
         * but must accept the requests without erroring. release destroys it. */
        if (opcode == WL_DD_RELEASE) wl_delete_obj(c, obj_id);
        break;

    /* ── data source ─────────────────────────────────────────────────── */
    case OBJ_DATA_SOURCE:
        if (opcode == WL_DS_DESTROY) wl_delete_obj(c, obj_id);
        break;

    /* ── wl_subcompositor ────────────────────────────────────────────── */
    case OBJ_SUBCOMPOSITOR:
        if (opcode == WL_SUBCOMP_GET_SUBSURFACE && args_len >= 12) {
            /* get_subsurface(new id sub, surface, parent): assign the subsurface
             * role to an EXISTING wl_surface. The new wl_subsurface object points
             * at that same wl_surface_t so set_position reaches it; blit composites
             * it at (parent.x+sub_x, parent.y+sub_y). */
            uint32_t sub_id, surf_id, parent_id;
            memcpy(&sub_id,    args,     4);
            memcpy(&surf_id,   args + 4, 4);
            memcpy(&parent_id, args + 8, 4);
            wl_obj_t *so = wl_find_obj_any(surf_id);
            wl_surface_t *s = (so && so->type == OBJ_SURFACE) ? so->data : NULL;
            if (s) {
                s->is_subsurface      = true;
                s->parent_surface_id  = parent_id;
                s->sub_x = 0; s->sub_y = 0;
            }
            wl_new_obj(c, sub_id, OBJ_SUBSURFACE, s);
        }
        break;

    /* ── wl_subsurface ───────────────────────────────────────────────── */
    case OBJ_SUBSURFACE: {
        wl_surface_t *s = obj ? obj->data : NULL;   /* the surface holding this role */
        if (opcode == WL_SUBSURF_SET_POSITION && args_len >= 8 && s) {
            memcpy(&s->sub_x, args,     4);
            memcpy(&s->sub_y, args + 4, 4);
        } else if (opcode == WL_SUBSURF_DESTROY) {
            if (s) s->is_subsurface = false;
            wl_delete_obj(c, obj_id);
        }
        /* place_above/below + set_sync/set_desync: accepted, no-op (single content
         * subsurface drawn after its parent in object order = correct stacking). */
        break;
    }

    /* ── wl_region ───────────────────────────────────────────────────── */
    case OBJ_REGION:
        /* op=0: destroy, op=1: add, op=2: subtract — we don't clip, just track */
        if (opcode == 0) wl_delete_obj(c, obj_id);
        /* add/subtract silently accepted */
        break;

    default:
        /* Unknown object — log with client fd and type for diagnosis */
        fprintf(stderr, "[wayland] unknown fd=%d obj=%u type=%d op=%u (owner_fd=%d)\n",
                c->fd, obj_id, (int)type, opcode, obj_owner ? obj_owner->fd : -1);
        break;
    }
}

/* ── Receive messages from a client ──────────────────────────────────────── */

/* Pending fd queue — holds all fds received via SCM_RIGHTS in one recvmsg */
#define PENDING_FD_MAX 8
static int g_pending_fds[PENDING_FD_MAX];
static int g_pending_fd_head = 0;
static int g_pending_fd_tail = 0;

static void recv_all_fds(struct msghdr *msgh) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(msgh); cm;
         cm = CMSG_NXTHDR(msgh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        int nfds = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)CMSG_DATA(cm);
        for (int i = 0; i < nfds; i++) {
            if (((g_pending_fd_tail + 1) % PENDING_FD_MAX) == g_pending_fd_head) {
                close(fds[i]);  /* queue full — drop */
            } else {
                g_pending_fds[g_pending_fd_tail] = fds[i];
                g_pending_fd_tail = (g_pending_fd_tail + 1) % PENDING_FD_MAX;
            }
        }
    }
}

static int pending_fd_pop(void) {
    if (g_pending_fd_head == g_pending_fd_tail) return -1;
    int fd = g_pending_fds[g_pending_fd_head];
    g_pending_fd_head = (g_pending_fd_head + 1) % PENDING_FD_MAX;
    return fd;
}

static void wl_client_recv(wl_client_t *c) {
    uint8_t     anc[CMSG_SPACE(sizeof(int) * PENDING_FD_MAX)];
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
            orphan_save_buffers(c);
            close(c->fd);
            c->fd     = -1;
            c->active = false;
            /* Keep c->n_objs and c->objs intact — Firefox reconnects to the same
             * compositor and sends messages for objects it created in the previous
             * connection. Mark as zombie so the objects stay findable. */
        }
        return;
    }
    c->recv_used += (int)n;

    /* Collect ALL ancillary fds into the pending queue (one per create_pool) */
    recv_all_fds(&msgh);

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

        wl_handle_msg(c, obj_id, opcode, args, args_len);

        /* fd assignment now happens inside create_pool handler via pending_fd_pop() */

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
/* Blit one surface at its computed screen position */
static int blit_one_surface(int ci, wl_surface_t *s, uint32_t obj_id, int do_log) {
    extern void console_paste_rect(const uint32_t *src, uint64_t dx, uint64_t dy,
                                    uint64_t w, uint64_t h);
    if (do_log)
        fprintf(stderr, "[blit] ci=%d obj=%u mapped=%d data=%p w=%d h=%d sub=%d\n",
                ci, obj_id, s->mapped, s->buf ? s->buf->data : NULL,
                s->w, s->h, s->is_subsurface);
    if (!s->mapped || !s->buf || !s->buf->data) return 0;
    if (s->w <= 0 || s->h <= 0) return 0;
    int32_t bx = s->x, by = s->y;
    if (s->is_subsurface) {
        wl_obj_t *po = wl_find_obj_any(s->parent_surface_id);
        wl_surface_t *p = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
        if (!p) return 0;
        bx = p->x + s->sub_x;
        by = p->y + s->sub_y;
    }
    console_paste_rect((const uint32_t *)s->buf->data,
                       (uint64_t)bx, (uint64_t)by,
                       (uint64_t)s->w, (uint64_t)s->h);
    return 1;
}

void wayland_blit_surfaces(void) {
    static int blit_log_ticker = 0;
    int do_log = (++blit_log_ticker % 600 == 0);
    int blitted = 0;

    /* Two-pass: parent surfaces first, then subsurfaces on top.
     * Without this, parents overwrite subsurface content. */
    for (int pass = 0; pass < 2; pass++) {
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            wl_client_t *c = &g_wl_clients[ci];
            if (!c->active) continue;
            for (int oi = 0; oi < c->n_objs; oi++) {
                if (c->objs[oi].type != OBJ_SURFACE) continue;
                wl_surface_t *s = c->objs[oi].data;
                if (!s) continue;
                /* pass 0 = non-subsurfaces, pass 1 = subsurfaces */
                if (pass == 0 && s->is_subsurface) continue;
                if (pass == 1 && !s->is_subsurface) continue;
                blitted += blit_one_surface(ci, s, c->objs[oi].id, do_log);
            }
        }
    }
    if (do_log) fprintf(stderr, "[blit] total drawn: %d\n", blitted);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Set display resolution — call once after framebuffer is known */
void wayland_set_display_size(int w, int h) {
    g_w = w; g_h = h;
}

/* Return the listening socket fd for inclusion in poll() */
int wayland_server_fd(void) { return g_wl_fd; }

/* Initialize: create Unix socket at $XDG_RUNTIME_DIR/wayland-0.
 * Falls back to a user-private directory under /tmp rather than the
 * shared /tmp root, to prevent other users from squatting the well-known path. */
bool wayland_init(void) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    static char fallback_dir[64];
    if (!xdg || !xdg[0]) {
        snprintf(fallback_dir, sizeof(fallback_dir), "/tmp/fifi-runtime-%d", (int)getuid());
        if (mkdir(fallback_dir, 0700) == 0 || errno == EEXIST)
            chmod(fallback_dir, 0700);
        xdg = fallback_dir;
    }
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
                /* Preserve objects array — Firefox reconnects and reuses object IDs
                 * from previous sessions. Only reset connection-state fields. */
                g_wl_clients[i].fd          = cfd;
                g_wl_clients[i].active      = true;
                g_wl_clients[i].serial      = 1;
                g_wl_clients[i].send_used   = 0;
                g_wl_clients[i].recv_used   = 0;
                g_wl_clients[i].send_overflow = false;
                /* Keep n_objs and objs[] intact for zombie-session object reuse */
                g_wl_clients[i].compositor_id = 0;
                g_wl_clients[i].shm_id        = 0;
                g_wl_clients[i].seat_id       = 0;
                g_wl_clients[i].keyboard_id   = 0;
                g_wl_clients[i].pointer_id    = 0;
                g_wl_clients[i].output_id     = 0;
                g_wl_clients[i].xdg_wm_id     = 0;
                fprintf(stderr, "[wayland] new client fd=%d slot=%d (objs=%d from prev)\n", cfd, i, g_wl_clients[i].n_objs);
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
