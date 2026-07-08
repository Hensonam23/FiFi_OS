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
#define XDG_TOPLEVEL_SET_MINIMIZED  13
/* xdg_toplevel.state values (for the configure states array) */
#define XDG_TOPLEVEL_STATE_MAXIMIZED  1
#define XDG_TOPLEVEL_STATE_FULLSCREEN 2
#define XDG_TOPLEVEL_STATE_RESIZING   3
#define XDG_TOPLEVEL_STATE_ACTIVATED  4

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

/* xdg_positioner opcodes */
#define XDG_POSITIONER_DESTROY                  0
#define XDG_POSITIONER_SET_SIZE                 1
#define XDG_POSITIONER_SET_ANCHOR_RECT          2
#define XDG_POSITIONER_SET_ANCHOR               3
#define XDG_POSITIONER_SET_GRAVITY              4
#define XDG_POSITIONER_SET_CONSTRAINT_ADJUSTMENT 5
#define XDG_POSITIONER_SET_OFFSET               6

/* xdg_popup opcodes (requests) */
#define XDG_POPUP_DESTROY    0
#define XDG_POPUP_GRAB       1
#define XDG_POPUP_REPOSITION 2
/* xdg_popup events */
#define XDG_POPUP_CONFIGURE  0
#define XDG_POPUP_DONE       1

/* zxdg_decoration_manager_v1 — server-side decorations. Granting SERVER mode
 * makes GTK/Firefox/Electron skip their own titlebars so every window on the
 * desktop wears the same FiFi chrome. */
#define ZXDG_DECO_MGR_DESTROY       0   /* request */
#define ZXDG_DECO_MGR_GET_TOPLEVEL  1   /* request: new_id, xdg_toplevel */
#define ZXDG_TL_DECO_DESTROY        0   /* request */
#define ZXDG_TL_DECO_SET_MODE       1   /* request: uint mode */
#define ZXDG_TL_DECO_UNSET_MODE     2   /* request */
#define ZXDG_TL_DECO_CONFIGURE      0   /* event: uint mode */
#define ZXDG_DECO_MODE_CLIENT       1
#define ZXDG_DECO_MODE_SERVER       2
#define SSD_TITLE_H                 24  /* server-side titlebar height */

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
    OBJ_POSITIONER,
    OBJ_XDG_POPUP,
    OBJ_DECO_MGR,
    OBJ_TL_DECO,        /* data ALIASES a wl_surface_t — never freed here */
    OBJ_KDE_DECO_MGR,   /* org_kde_kwin_server_decoration_manager (GTK3/Firefox) */
    OBJ_KDE_DECO,       /* org_kde_kwin_server_decoration; data ALIASES a wl_surface_t */
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

/* ── Positioner ───────────────────────────────────────────────────────────── */
typedef struct {
    int32_t  w, h;
    int32_t  ar_x, ar_y, ar_w, ar_h;
    int32_t  off_x, off_y;
} xdg_positioner_t;

/* ── Surface ──────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t     buffer_id;   /* 0 = no buffer attached */
    bool         has_new_buffer; /* a new buffer was attached since the last commit */
    wl_shm_buf_t *buf;        /* transient: client buffer located during commit (not kept) */
    /* Compositor-owned copy of the committed pixels. We copy at commit and release
     * the client buffer immediately, so our rendering never depends on client buffer
     * lifetime (no use-after-free, no premature-release crashes, no stale dims). */
    uint32_t    *own_pix;
    size_t       own_cap;     /* allocated bytes of own_pix */
    int32_t      own_w, own_h;/* dimensions of the owned copy */
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
    /* xdg_surface window geometry — the "visible" content area within the surface.
     * Used to shift the blit so shadows/decorations extend off-screen rather than
     * pushing the content off the right/bottom edge. */
    int32_t      geom_x, geom_y;  /* offset of content area within surface (shadow size) */
    int32_t      geom_w, geom_h;  /* size of content area (0 = whole buffer) */
    uint32_t     surface_id;       /* this surface's own wl_surface object ID */
    /* Pending frame callback: fired during commit (not on frame request).
     * Firing immediately on frame request causes clients to destroy buffers
     * before we process the commit, breaking the render pipeline. */
    uint32_t     pending_frame_cb;
    /* xdg_popup role */
    bool         is_popup;
    uint32_t     xdg_popup_id;
    int32_t      popup_x, popup_y;  /* absolute screen position */
    bool         popup_has_grab;
    /* Toplevel window state (interactive move/resize + maximize/fullscreen/minimize) */
    bool         maximized, fullscreen, minimized;
    bool         half_snapped;   /* Super+Left/Right half-screen snap */
    bool         ssd;               /* server-side decorations granted (FiFi chrome) */
    uint32_t     deco_id;           /* zxdg_toplevel_decoration object, 0 = none */
    bool         placed;            /* initial window placement done */
    int32_t      restore_x, restore_y, restore_w, restore_h;  /* saved windowed geom */
    /* Pending destroy: set by DESTROY, cleaned up on next COMMIT so Firefox can
     * receive buffer_release before the surface is gone (fixes tab-close crash). */
    bool         pending_destroy;
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

/* Exact live-set of currently-allocated buffer/pool structs (wl_shm_buf_t).
 * The orphan pool + zombie slot-reuse across Firefox's multiple client
 * connections can leave the same pointer reachable from more than one route;
 * freeing it twice aborts (double free in tcache). We track every wl_shm_buf_t
 * at allocation and free it ONLY if still tracked, removing it atomically — so a
 * second (dangling) free is a no-op. A reused heap address is re-tracked on its
 * next allocation, so this never wrongly skips a legitimate free. */
#define MAX_LIVE_BUFS 8192
static void *g_live_bufs[MAX_LIVE_BUFS];
static int   g_n_live = 0;
static void track_buf(void *p) {
    if (!p) return;
    /* Dedup: a heap address reused after a free-that-didn't-untrack could leave a
     * stale entry; never let the same pointer sit in the set twice or a later free
     * sees it "live" after glibc already freed it (double-free abort). */
    for (int i = 0; i < g_n_live; i++)
        if (g_live_bufs[i] == p) return;   /* already tracked */
    if (g_n_live < MAX_LIVE_BUFS) g_live_bufs[g_n_live++] = p;
    /* else: table full — drop (extremely unlikely at MAX_LIVE_BUFS) */
}
/* Remove ALL instances of p; returns true if it was tracked. */
static bool untrack_buf(void *p) {
    bool found = false;
    for (int i = 0; i < g_n_live; ) {
        if (g_live_bufs[i] == p) { g_live_bufs[i] = g_live_bufs[--g_n_live]; found = true; }
        else i++;
    }
    return found;
}

static void free_obj_data(wl_obj_t *o) {
    if (!o->data) return;
    if (o->type == OBJ_SURFACE) {
        wl_surface_t *s = o->data;
        if (s->own_pix) free(s->own_pix);  /* free the compositor-owned pixel copy */
        free(o->data);
    } else if (o->type == OBJ_SHM_POOL || o->type == OBJ_BUFFER) {
        wl_shm_buf_t *b = o->data;
        /* Drop any other reference to this exact pointer (other slots, orphan pool)
         * so nothing tries to use it after free. */
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            for (int oi = 0; oi < g_wl_clients[ci].n_objs; oi++) {
                wl_obj_t *other = &g_wl_clients[ci].objs[oi];
                if (other != o && other->data == b) { other->data = NULL; other->type = OBJ_NONE; }
            }
        }
        for (int i = 0; i < g_n_orphans; i++)
            if (g_orphan_bufs[i].buf == b) g_orphan_bufs[i].buf = NULL;
        /* Free exactly once: only if still in the live-set. */
        if (untrack_buf(b)) {
            if (b->data && b->size) { munmap(b->data, b->size); b->data = NULL; }
            if (b->fd >= 0) { close(b->fd); b->fd = -1; }
            free(b);
        }
        /* else: already freed via another reference — skip silently (no abort). */
    } else if (o->type == OBJ_SUBSURFACE || o->type == OBJ_TL_DECO ||
               o->type == OBJ_KDE_DECO) {
        /* A subsurface / decoration is only a ROLE handle aliasing a wl_surface_t
         * owned by that surface's OBJ_SURFACE slot. It does NOT own the pointer —
         * freeing it here double-frees the surface. Drop the alias. */
    } else if (o->data) {
        free(o->data);
    }
    o->data = NULL;
}

/* Copy a client SHM buffer's pixels into the surface's own packed copy.
 * Handles row stride and bounds the read to the actual mapped size so a
 * smaller/short buffer can never cause an out-of-bounds read. */
static void surface_copy_buffer(wl_surface_t *s, wl_shm_buf_t *b) {
    if (!b || !b->data || b->width <= 0 || b->height <= 0) return;
    int64_t w = b->width, h = b->height;
    int64_t stride_px = b->stride > 0 ? b->stride / 4 : w;
    if (stride_px < w) stride_px = w;
    if (b->size) {                              /* never read past the mmap */
        int64_t max_rows = (int64_t)(b->size / (size_t)(stride_px * 4));
        if (h > max_rows) h = max_rows;
    }
    if (h <= 0) return;
    size_t need = (size_t)(w * h * 4);
    if (s->own_cap < need) {
        uint32_t *p = realloc(s->own_pix, need);
        if (!p) return;                         /* keep old copy on OOM */
        s->own_pix = p;
        s->own_cap = need;
    }
    const uint32_t *src = (const uint32_t *)b->data;
    for (int64_t row = 0; row < h; row++)
        memcpy(s->own_pix + row * w, src + row * stride_px, (size_t)(w * 4));
    s->own_w = (int32_t)w;
    s->own_h = (int32_t)h;
    s->mapped = true;
}

static wl_obj_t *wl_new_obj(wl_client_t *c, uint32_t id, obj_type_t type, void *data) {
    /* If this ID already exists (from a previous session or re-use), overwrite it */
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].id == id) {
            /* Properly free old data to prevent double-free and leaks */
            free_obj_data(&c->objs[i]);
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
            free_obj_data(&c->objs[i]);
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
    send_registry_global(c, reg_id,  8, "zxdg_decoration_manager_v1", 1);
    send_registry_global(c, reg_id,  9, "org_kde_kwin_server_decoration_manager", 1);
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

/* Send an xdg_toplevel + xdg_surface configure with a specific size and up to two
 * extra states (0 = none). ACTIVATED is always included so Firefox accepts input. */
static void send_toplevel_configure(wl_client_t *c, wl_surface_t *s,
                                     int32_t cw, int32_t ch,
                                     uint32_t st1, uint32_t st2) {
    uint32_t ser = next_serial(c);
    s->serial = ser;
    if (c->output_id && s->surface_id) {
        int h0 = wl_begin_msg(c, s->surface_id, WL_SURFACE_ENTER);
        wl_push_u32(c, c->output_id);
        wl_end_msg(c, h0);
    }
    int h = wl_begin_msg(c, s->xdg_toplevel_id, XDG_TOPLEVEL_CONFIGURE);
    wl_push_u32(c, (uint32_t)(cw > 0 ? cw : g_w));
    wl_push_u32(c, (uint32_t)(ch > 0 ? ch : g_h));
    uint32_t n = 1 + (st1 ? 1 : 0) + (st2 ? 1 : 0);  /* ACTIVATED + extras */
    wl_push_u32(c, n * 4);                            /* states array byte length */
    if (st1) wl_push_u32(c, st1);
    if (st2) wl_push_u32(c, st2);
    wl_push_u32(c, XDG_TOPLEVEL_STATE_ACTIVATED);
    wl_end_msg(c, h);
    h = wl_begin_msg(c, s->xdg_surface_id, XDG_SURFACE_CONFIGURE);
    wl_push_u32(c, ser);
    wl_end_msg(c, h);
}

static void send_xdg_surface_configure(wl_client_t *c, wl_surface_t *s) {
    uint32_t st = s->fullscreen ? XDG_TOPLEVEL_STATE_FULLSCREEN
                : s->maximized  ? XDG_TOPLEVEL_STATE_MAXIMIZED : 0;
    send_toplevel_configure(c, s, s->w, s->h, st, 0);
}

static void wl_client_flush(wl_client_t *c);

/* Grant server-side decorations: the client drops its own titlebar and the
 * compositor draws the FiFi chrome instead. The window is re-placed as a
 * centered floating window with room for the bar above it. */
static void deco_grant_ssd(wl_client_t *c, wl_surface_t *s) {
    if (!s->deco_id) return;
    /* SERVER-side decorations: the compositor draws the FiFi titlebar for every
     * app, so downloaded apps match the built-in windows. Toolkit apps (GTK/Qt)
     * and framed Electron (Bitwarden) hide their own titlebar and show only ours.
     * Frameless apps (Obsidian) render window buttons inside their web content —
     * no compositor can remove those — but our chrome still works on them.
     * We do NOT force maximize (that made Electron spawn a transparent full-screen
     * host surface that stole clicks); the window keeps its own size. */
    s->ssd = true;
    int h = wl_begin_msg(c, s->deco_id, ZXDG_TL_DECO_CONFIGURE);
    wl_push_u32(c, ZXDG_DECO_MODE_SERVER);
    wl_end_msg(c, h);
    send_xdg_surface_configure(c, s);
    wl_client_flush(c);
}

/* A toplevel we should draw FiFi chrome on: an SSD toplevel with a real window
 * buffer. Two transparent-center cases must be told apart:
 *   - Electron's phantom "host": a FULL-SCREEN transparent surface → must NOT be
 *     decorated (would put a titlebar over the whole screen and steal clicks).
 *   - Firefox/LibreWolf: a normal window-sized toplevel that's transparent in the
 *     center because its content lives on a subsurface → SHOULD be decorated.
 * So: decorate if the center is opaque, OR it's transparent but smaller than the
 * full display (a real window, not the phantom host). */
static bool ssd_decorated(const wl_surface_t *s) {
    if (!s || !s->ssd || s->is_popup || s->is_subsurface || !s->xdg_toplevel_id)
        return false;
    if (!s->mapped || s->minimized || !s->own_pix || s->own_w < 8 || s->own_h < 8)
        return false;
    uint32_t px = s->own_pix[(int64_t)(s->own_h / 2) * (int64_t)s->own_w + s->own_w / 2];
    if ((px >> 24) != 0) return true;                 /* opaque center → real window */
    return (s->own_w < g_w || s->own_h < g_h);        /* transparent but not full-screen → real window */
}

/* Decoration objects can be created BEFORE xdg_surface.get_toplevel links the
 * surface (Electron does this). Track unattached decorations by toplevel id and
 * attach them when the toplevel appears. */
#define MAX_PENDING_DECO 8
static struct { uint32_t deco_id, tl_id; } g_pending_deco[MAX_PENDING_DECO];
static int g_n_pending_deco = 0;

static bool deco_try_attach(wl_client_t *c, uint32_t deco_id, uint32_t tl_id) {
    for (int i = 0; i < c->n_objs; i++) {
        if (c->objs[i].type != OBJ_SURFACE || !c->objs[i].data) continue;
        wl_surface_t *cand = c->objs[i].data;
        if (cand->xdg_toplevel_id != tl_id) continue;
        wl_obj_t *dobj = wl_find_obj(c, deco_id);
        if (dobj && dobj->type == OBJ_TL_DECO) dobj->data = cand;
        cand->deco_id = deco_id;
        deco_grant_ssd(c, cand);
        return true;
    }
    return false;
}

static void deco_attach_pending(wl_client_t *c, uint32_t tl_id) {
    for (int i = 0; i < g_n_pending_deco; ) {
        if (g_pending_deco[i].tl_id == tl_id &&
            deco_try_attach(c, g_pending_deco[i].deco_id, tl_id)) {
            g_pending_deco[i] = g_pending_deco[--g_n_pending_deco];
        } else i++;
    }
}

static void send_xdg_popup_configure(wl_client_t *c, wl_surface_t *s,
                                      int32_t px, int32_t py,
                                      int32_t pw, int32_t ph) {
    uint32_t ser = next_serial(c);
    s->serial = ser;
    int h = wl_begin_msg(c, s->xdg_popup_id, XDG_POPUP_CONFIGURE);
    wl_push_u32(c, (uint32_t)px);
    wl_push_u32(c, (uint32_t)py);
    wl_push_u32(c, (uint32_t)pw);
    wl_push_u32(c, (uint32_t)ph);
    wl_end_msg(c, h);
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

/* Prefer a fully-resolved keymap file (no include directives) when present:
 * some clients (Chromium/Electron) compile the keymap with an xkb context that
 * cannot resolve includes, end up with no XKB state, and segfault on the first
 * key event. /fifi-data/fifi-keymap.txt is a complete `xkbcli compile-keymap`
 * dump; the minimal include-based string remains the fallback. */
static char  *g_full_keymap     = NULL;
static bool   g_full_keymap_try = false;

static const char *keymap_str(void) {
    if (!g_full_keymap_try) {
        g_full_keymap_try = true;
        FILE *f = fopen("/fifi-data/fifi-keymap.txt", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 128 && sz < 4*1024*1024) {
                g_full_keymap = malloc((size_t)sz + 1);
                if (g_full_keymap) {
                    if (fread(g_full_keymap, 1, (size_t)sz, f) == (size_t)sz) {
                        g_full_keymap[sz] = '\0';
                        fprintf(stderr, "[wayland] using full keymap (%ld bytes)\n", sz);
                    } else { free(g_full_keymap); g_full_keymap = NULL; }
                }
            }
            fclose(f);
        }
    }
    return g_full_keymap ? g_full_keymap : s_keymap;
}

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
    const char *kmap = keymap_str();
    size_t klen = strlen(kmap) + 1;
    if (write(kfd, kmap, klen) != (ssize_t)klen) { close(kfd); return; }
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

/* Send wl_buffer.release for buffer_id to whichever client owns it. */
static void wl_release_buffer(uint32_t buffer_id) {
    if (!buffer_id) return;
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        if (!g_wl_clients[ci].active) continue;
        if (wl_find_obj(&g_wl_clients[ci], buffer_id)) {
            int h = wl_begin_msg(&g_wl_clients[ci], buffer_id, WL_BUFFER_RELEASE);
            wl_end_msg(&g_wl_clients[ci], h);
            return;
        }
    }
}

/* ── Focus + interactive-op state (used by dispatch and the mouse handler) ──── */
static int      g_focus_ci  = -1;  /* client index with POINTER focus */
static uint32_t g_focus_sid = 0;   /* surface obj id with POINTER focus */
/* Keyboard focus is tracked separately: it follows only real toplevels, never a
 * popup/menu. Moving the pointer onto a menu must NOT send the toplevel a
 * keyboard-leave — Firefox reads that as the window deactivating and instantly
 * dismisses its (non-grabbing) menu. */
static int      g_kbd_ci    = -1;
static uint32_t g_kbd_sid   = 0;
static int32_t  g_prev_mx = -1, g_prev_my = -1;
static uint8_t  g_prev_btns = 0;

/* Interactive move/resize state (driven by xdg_toplevel.move / .resize) */
static int      g_iop = 0;          /* 0=none 1=move 2=resize */
static int      g_iop_ci = -1;
static uint32_t g_iop_sid = 0;      /* wl_surface obj id being manipulated */
static int32_t  g_iop_sx, g_iop_sy; /* mouse pos at op start */
static int32_t  g_iop_ox, g_iop_oy, g_iop_ow, g_iop_oh;  /* surface geom at op start */
static uint32_t g_iop_edges;        /* resize edges */

/* Find the wl_surface_t + its client index that owns a given xdg_toplevel id. */
static wl_surface_t *find_surface_by_toplevel(uint32_t tl_id, int *out_ci) {
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        for (int oi = 0; oi < g_wl_clients[ci].n_objs; oi++) {
            if (g_wl_clients[ci].objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = g_wl_clients[ci].objs[oi].data;
            if (s && s->xdg_toplevel_id == tl_id) { if (out_ci) *out_ci = ci; return s; }
        }
    }
    return NULL;
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
            } else if (name == 8 && strncmp(iface, "zxdg_decoration_manager_v1", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_DECO_MGR, NULL);
            } else if (name == 9 && strncmp(iface, "org_kde_kwin_server_decoration_manager", iface_len) == 0) {
                wl_new_obj(c, new_id, OBJ_KDE_DECO_MGR, NULL);
                /* default_mode(Server=2): hint SSD so GTK3/Firefox drops its CSD. */
                int dh = wl_begin_msg(c, new_id, 0 /* default_mode */);
                wl_push_u32(c, 2 /* Server */);
                wl_end_msg(c, dh);
                wl_client_flush(c);
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
            s->surface_id = sid;
            wl_new_obj(c, sid, OBJ_SURFACE, s);
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
            s->has_new_buffer = true;  /* consume + release exactly once on next commit */
            /* dx, dy at args+4 and args+8 — ignore for now */
        } else if (opcode == WL_SURFACE_COMMIT) {
            /* Tab-close fix: DESTROY set pending_destroy; clean up on commit. */
            if (s->pending_destroy) {
                if (s->buffer_id) wl_release_buffer(s->buffer_id);
                wl_delete_obj(c, obj_id);
                break;
            }
            /* Copy-at-commit, but ONLY when a new buffer was actually attached.
             * A client may commit without attaching (damage-only / frame-driven);
             * releasing again then would double-release a buffer GDK has already
             * taken back and reused as its staging surface → cairo assertion + crash.
             * Locate the buffer (Firefox creates it in one process and commits the
             * surface in another, so search all clients + orphan pool), copy its
             * pixels into our OWN store, then release the client buffer exactly once. */
            if (s->has_new_buffer) {
                s->has_new_buffer = false;
                if (s->buffer_id) {
                    wl_obj_t *bobj = wl_find_obj_any(s->buffer_id);
                    wl_shm_buf_t *src = (bobj && bobj->type == OBJ_BUFFER) ? bobj->data : NULL;
                    if (!src) src = orphan_find(s->buffer_id);
                    if (src && src->data) {
                        surface_copy_buffer(s, src);  /* sets own_pix/own_w/own_h, mapped */
                        /* SSD windows: keep the FiFi titlebar on-screen and the
                         * window within the desktop area whatever size the client
                         * actually committed. */
                        if (s->ssd && s->xdg_toplevel_id && !s->is_popup && !s->is_subsurface) {
                            extern uint32_t console_font_height(void);
                            int32_t fh3  = (int32_t)console_font_height();
                            int32_t top3 = fh3 + 6 + SSD_TITLE_H;
                            if (s->x + s->own_w > g_w) s->x = g_w - s->own_w;
                            if (s->x < 0) s->x = 0;
                            int32_t bot3 = g_h - (fh3 + 10);
                            if (s->y + s->own_h > bot3) s->y = bot3 - s->own_h;
                            if (s->y < top3) s->y = top3;
                        }
                        if (src->width > 0 && src->height > 0) {
                            s->w = src->width; s->h = src->height;
                        }
                    }
                    wl_release_buffer(s->buffer_id);  /* exactly once per attach */
                } else {
                    s->mapped = false;  /* NULL attach = unmap */
                }
            }
            /* Fire pending frame callback (stored at WL_SURFACE_FRAME time) */
            if (s->pending_frame_cb) {
                send_wl_callback_done(c, s->pending_frame_cb, g_global_serial++);
                s->pending_frame_cb = 0;
            }
        } else if (opcode == WL_SURFACE_FRAME && args_len >= 4) {
            uint32_t cb_id; memcpy(&cb_id, args, 4);
            wl_new_obj(c, cb_id, OBJ_CALLBACK, NULL);
            /* Store callback — fire it during commit so the client doesn't
             * free buffers before we've had a chance to process the commit. */
            if (s) s->pending_frame_cb = cb_id;
        } else if (opcode == WL_SURFACE_DESTROY) {
            s->mapped = false;
            s->pending_destroy = true;
            /* Defer delete until next commit so Firefox receives buffer_release first */
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
            track_buf(pool);
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
            track_buf(buf);
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
            /* Keep the FIRST keyboard — Firefox creates two (second is GPU process).
             * Input events go to the first (main process) keyboard; second discards. */
            if (!c->keyboard_id) { c->keyboard_id = kid; send_keymap(c); }
        } else if (opcode == WL_SEAT_GET_POINTER && args_len >= 4) {
            uint32_t pid; memcpy(&pid, args, 4);
            wl_new_obj(c, pid, OBJ_POINTER, NULL);
            /* Keep the FIRST pointer — Firefox creates two; input goes to first */
            if (!c->pointer_id) c->pointer_id = pid;
        }
        break;

    /* ── xdg_wm_base ─────────────────────────────────────────────────── */
    case OBJ_XDG_WM_BASE:
        if (opcode == XDG_WM_BASE_PONG) {
            /* client responded to our ping — good */
        } else if (opcode == XDG_WM_BASE_CREATE_POSITIONER && args_len >= 4) {
            uint32_t pos_id; memcpy(&pos_id, args, 4);
            xdg_positioner_t *pos = calloc(1, sizeof(xdg_positioner_t));
            wl_new_obj(c, pos_id, OBJ_POSITIONER, pos);
        } else if (opcode == XDG_WM_BASE_GET_XDG_SURFACE && args_len >= 8) {
            uint32_t xdg_surf_id, surf_id;
            memcpy(&xdg_surf_id, args,   4);
            memcpy(&surf_id,     args+4, 4);
            wl_new_obj(c, xdg_surf_id, OBJ_XDG_SURFACE, NULL);
            /* link surface to its xdg_surface — SAME client first: object IDs are
             * per-connection, so an any-client lookup can hit another app's surface
             * when several Wayland apps run at once (windows then never map). */
            wl_obj_t *so = wl_find_obj(c, surf_id);
            if (!so || so->type != OBJ_SURFACE) so = wl_find_obj_any(surf_id);
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
                        /* Place floating + centered below the status bar. NOT
                         * maximized: forcing maximize made Electron spawn a
                         * transparent full-screen host surface. The FiFi titlebar
                         * is drawn just above the surface top (see ssd_draw_chrome). */
                        if (!s->placed) {
                            s->placed = true;
                            extern uint32_t console_font_height(void);
                            int32_t fhh = (int32_t)console_font_height();
                            int32_t status = fhh + 6, taskbar = fhh + 10;
                            int32_t top = status + SSD_TITLE_H;
                            int32_t avail_h = g_h - top - taskbar;
                            int32_t rw = g_w * 78 / 100, rh = avail_h * 88 / 100;
                            if (rw > 1500) rw = 1500;
                            if (rh > 950)  rh = 950;
                            s->restore_w = rw; s->restore_h = rh;
                            s->restore_x = (g_w - rw) / 2;
                            s->restore_y = top + (avail_h - rh) / 2;
                            s->maximized = false;
                            s->x = s->restore_x; s->y = s->restore_y;
                            send_toplevel_configure(c, s, rw, rh, 0, 0);
                        } else {
                            send_xdg_surface_configure(c, s);
                        }
                        deco_attach_pending(c, tl_id);   /* decoration created early? */
                        break;
                    }
                }
            }
        } else if (opcode == XDG_SURFACE_SET_WINDOW_GEOMETRY && args_len >= 16) {
            /* Content rect within the surface buffer. The area OUTSIDE it is the CSD
             * shadow (transparent) — we crop to this rect in the blit so the shadow
             * isn't painted as a black border. */
            int32_t gx, gy, gw, gh;
            memcpy(&gx, args,      4);
            memcpy(&gy, args + 4,  4);
            memcpy(&gw, args + 8,  4);
            memcpy(&gh, args + 12, 4);
            for (int i = 0; i < c->n_objs; i++) {
                if (c->objs[i].type == OBJ_SURFACE && c->objs[i].data) {
                    wl_surface_t *s = c->objs[i].data;
                    if (s->xdg_surface_id == obj_id) {
                        s->geom_x = gx; s->geom_y = gy;
                        s->geom_w = gw; s->geom_h = gh;
                        break;
                    }
                }
            }
        } else if (opcode == XDG_SURFACE_GET_POPUP && args_len >= 12) {
            uint32_t popup_id, parent_xdg_id, positioner_id;
            memcpy(&popup_id,      args,   4);
            memcpy(&parent_xdg_id, args+4, 4);
            memcpy(&positioner_id, args+8, 4);
            wl_new_obj(c, popup_id, OBJ_XDG_POPUP, NULL);
            for (int i = 0; i < c->n_objs; i++) {
                if (c->objs[i].type != OBJ_SURFACE || !c->objs[i].data) continue;
                wl_surface_t *s = c->objs[i].data;
                if (s->xdg_surface_id != obj_id) continue;
                (void)parent_xdg_id;
                s->xdg_popup_id = popup_id;
                s->is_popup     = true;
                wl_obj_t *po = wl_find_obj(c, positioner_id);
                xdg_positioner_t *pos = (po && po->type == OBJ_POSITIONER) ? po->data : NULL;
                int32_t pw = 200, ph = 100, rel_x = 0, rel_y = 0;
                if (pos) {
                    if (pos->w > 0) pw = pos->w;
                    if (pos->h > 0) ph = pos->h;
                    rel_x = pos->ar_x + pos->off_x;
                    rel_y = pos->ar_y + pos->ar_h + pos->off_y;
                }
                s->w = pw; s->h = ph;
                /* Place the popup relative to its parent surface using the
                 * positioner's anchor rect (the spec-correct approach): the
                 * popup's top-left goes at parent_content_origin + anchor +
                 * offset. The parent may be the toplevel (screen x/y) or another
                 * popup (its popup_x/y). Fall back to the cursor if the parent
                 * xdg_surface can't be resolved. */
                wl_surface_t *par = NULL;
                for (int j = 0; j < c->n_objs; j++) {
                    if (c->objs[j].type != OBJ_SURFACE || !c->objs[j].data) continue;
                    wl_surface_t *cand = c->objs[j].data;
                    if (cand->xdg_surface_id == parent_xdg_id) { par = cand; break; }
                }
                int32_t par_ox, par_oy;
                if (par) {
                    par_ox = par->is_popup ? par->popup_x : par->x;
                    par_oy = par->is_popup ? par->popup_y : par->y;
                } else {
                    /* fallback: cursor = par_o + rel  ->  par_o = cursor - rel */
                    par_ox = g_prev_mx - rel_x;
                    par_oy = g_prev_my - rel_y;
                }
                s->popup_x = par_ox + rel_x;
                s->popup_y = par_oy + rel_y;
                if (s->popup_x + pw > g_w) s->popup_x = g_w - pw;
                if (s->popup_y + ph > g_h) s->popup_y = g_h - ph;
                if (s->popup_x < 0) s->popup_x = 0;
                if (s->popup_y < 0) s->popup_y = 0;
                send_xdg_popup_configure(c, s, rel_x, rel_y, pw, ph);
                break;
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
        } else if (opcode == XDG_TOPLEVEL_MOVE) {
            /* Interactive move: args = seat(4), serial(4). Start tracking the drag. */
            int mci = -1; wl_surface_t *s = find_surface_by_toplevel(obj_id, &mci);
            if (s && !s->maximized && !s->fullscreen) {
                g_iop = 1; g_iop_ci = mci; g_iop_sid = s->surface_id;
                g_iop_sx = g_prev_mx; g_iop_sy = g_prev_my;
                g_iop_ox = s->x; g_iop_oy = s->y;
            }
        } else if (opcode == XDG_TOPLEVEL_RESIZE && args_len >= 12) {
            /* Interactive resize: args = seat(4), serial(4), edges(4). */
            uint32_t edges; memcpy(&edges, args + 8, 4);
            int mci = -1; wl_surface_t *s = find_surface_by_toplevel(obj_id, &mci);
            if (s && !s->maximized && !s->fullscreen) {
                g_iop = 2; g_iop_ci = mci; g_iop_sid = s->surface_id;
                g_iop_sx = g_prev_mx; g_iop_sy = g_prev_my;
                g_iop_ox = s->x; g_iop_oy = s->y; g_iop_ow = s->w; g_iop_oh = s->h;
                g_iop_edges = edges;
            }
        } else if (opcode == XDG_TOPLEVEL_SET_MAXIMIZED) {
            wl_surface_t *s = find_surface_by_toplevel(obj_id, NULL);
            if (s && !s->maximized) {
                if (!s->fullscreen) { s->restore_x = s->x; s->restore_y = s->y;
                                      s->restore_w = s->w; s->restore_h = s->h; }
                s->maximized = true; s->x = 0; s->y = 0;
                send_toplevel_configure(c, s, g_w, g_h, XDG_TOPLEVEL_STATE_MAXIMIZED, 0);
            }
        } else if (opcode == XDG_TOPLEVEL_UNSET_MAXIMIZED) {
            wl_surface_t *s = find_surface_by_toplevel(obj_id, NULL);
            if (s && s->maximized) {
                s->maximized = false;
                s->x = s->restore_x; s->y = s->restore_y;
                send_toplevel_configure(c, s, s->restore_w, s->restore_h, 0, 0);
            }
        } else if (opcode == XDG_TOPLEVEL_SET_FULLSCREEN) {
            wl_surface_t *s = find_surface_by_toplevel(obj_id, NULL);
            if (s && !s->fullscreen) {
                if (!s->maximized) { s->restore_x = s->x; s->restore_y = s->y;
                                     s->restore_w = s->w; s->restore_h = s->h; }
                s->fullscreen = true; s->x = 0; s->y = 0;
                send_toplevel_configure(c, s, g_w, g_h, XDG_TOPLEVEL_STATE_FULLSCREEN, 0);
            }
        } else if (opcode == XDG_TOPLEVEL_UNSET_FULLSCREEN) {
            wl_surface_t *s = find_surface_by_toplevel(obj_id, NULL);
            if (s && s->fullscreen) {
                s->fullscreen = false;
                if (s->maximized) { s->x = 0; s->y = 0;
                    send_toplevel_configure(c, s, g_w, g_h, XDG_TOPLEVEL_STATE_MAXIMIZED, 0);
                } else {
                    s->x = s->restore_x; s->y = s->restore_y;
                    send_toplevel_configure(c, s, s->restore_w, s->restore_h, 0, 0);
                }
            }
        } else if (opcode == XDG_TOPLEVEL_SET_MINIMIZED) {
            /* No-op for now: hiding the surface would strand the user (the FiFi
             * taskbar has no entry to restore a Wayland window). Needs taskbar
             * integration — deferred. The minimized field/blit-skip is ready. */
        } else if (opcode == XDG_TOPLEVEL_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;

    /* ── xdg_positioner ─────────────────────────────────────────────── */
    case OBJ_POSITIONER: {
        xdg_positioner_t *pos = obj ? obj->data : NULL;
        if (!pos) break;
        if (opcode == XDG_POSITIONER_SET_SIZE && args_len >= 8) {
            memcpy(&pos->w, args,   4);
            memcpy(&pos->h, args+4, 4);
        } else if (opcode == XDG_POSITIONER_SET_ANCHOR_RECT && args_len >= 16) {
            memcpy(&pos->ar_x, args,    4);
            memcpy(&pos->ar_y, args+4,  4);
            memcpy(&pos->ar_w, args+8,  4);
            memcpy(&pos->ar_h, args+12, 4);
        } else if (opcode == XDG_POSITIONER_SET_OFFSET && args_len >= 8) {
            memcpy(&pos->off_x, args,   4);
            memcpy(&pos->off_y, args+4, 4);
        } else if (opcode == XDG_POSITIONER_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        /* SET_ANCHOR / SET_GRAVITY / SET_CONSTRAINT_ADJUSTMENT: accept, no-op */
        break;
    }

    /* ── xdg_popup ──────────────────────────────────────────────────── */
    case OBJ_XDG_POPUP: {
        wl_surface_t *ps = NULL;
        for (int i = 0; i < c->n_objs; i++) {
            if (c->objs[i].type == OBJ_SURFACE && c->objs[i].data) {
                wl_surface_t *_s = c->objs[i].data;
                if (_s->xdg_popup_id == obj_id) { ps = _s; break; }
            }
        }
        if (opcode == XDG_POPUP_GRAB) {
            if (ps) ps->popup_has_grab = true;
        } else if (opcode == XDG_POPUP_DESTROY) {
            if (ps) { ps->is_popup = false; ps->popup_has_grab = false; ps->mapped = false; }
            wl_delete_obj(c, obj_id);
        }
        break;
    }

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
    /* ── zxdg_decoration_manager_v1 ──────────────────────────────────── */
    case OBJ_DECO_MGR:
        if (opcode == ZXDG_DECO_MGR_GET_TOPLEVEL && args_len >= 8) {
            uint32_t deco_id, tl_id;
            memcpy(&deco_id, args,     4);
            memcpy(&tl_id,   args + 4, 4);
            wl_new_obj(c, deco_id, OBJ_TL_DECO, NULL);
            /* Answer the mode immediately — clients block their first commit on it.
             * SERVER mode: the compositor draws the FiFi titlebar (see deco_grant_ssd). */
            int dh = wl_begin_msg(c, deco_id, ZXDG_TL_DECO_CONFIGURE);
            wl_push_u32(c, ZXDG_DECO_MODE_SERVER);
            wl_end_msg(c, dh);
            if (!deco_try_attach(c, deco_id, tl_id) && g_n_pending_deco < MAX_PENDING_DECO) {
                g_pending_deco[g_n_pending_deco].deco_id = deco_id;
                g_pending_deco[g_n_pending_deco].tl_id   = tl_id;
                g_n_pending_deco++;
                fprintf(stderr, "[wayland] deco pending for toplevel %u\n", tl_id);
            }
        } else if (opcode == ZXDG_DECO_MGR_DESTROY) {
            wl_delete_obj(c, obj_id);
        }
        break;

    /* ── zxdg_toplevel_decoration_v1 ─────────────────────────────────── */
    case OBJ_TL_DECO: {
        wl_surface_t *ds = obj ? obj->data : NULL;
        if (opcode == ZXDG_TL_DECO_SET_MODE || opcode == ZXDG_TL_DECO_UNSET_MODE) {
            /* whatever the client prefers, we impose server-side (uniform chrome) */
            if (ds) deco_grant_ssd(c, ds);
        } else if (opcode == ZXDG_TL_DECO_DESTROY) {
            if (ds) { ds->ssd = false; ds->deco_id = 0; }
            wl_delete_obj(c, obj_id);
        }
        break;
    }

    /* ── org_kde_kwin_server_decoration_manager (GTK3/Firefox use this) ──── */
    case OBJ_KDE_DECO_MGR:
        if (opcode == 0 /* create(new_id, surface) */ && args_len >= 8) {
            uint32_t kdeco_id, surf_id;
            memcpy(&kdeco_id, args,     4);
            memcpy(&surf_id,  args + 4, 4);
            wl_obj_t *so = wl_find_obj(c, surf_id);
            wl_surface_t *s = (so && so->type == OBJ_SURFACE) ? so->data : NULL;
            wl_new_obj(c, kdeco_id, OBJ_KDE_DECO, s);
            /* Impose Server mode so the toolkit hides its own titlebar and we
             * draw the FiFi chrome (ssd_decorated keys off s->ssd). */
            if (s) s->ssd = true;
            int mh = wl_begin_msg(c, kdeco_id, 0 /* mode */);
            wl_push_u32(c, 2 /* Server */);
            wl_end_msg(c, mh);
            wl_client_flush(c);
        }
        break;

    /* ── org_kde_kwin_server_decoration ──────────────────────────────────── */
    case OBJ_KDE_DECO: {
        wl_surface_t *ks = obj ? obj->data : NULL;
        if (opcode == 1 /* request_mode(mode) */) {
            if (ks) ks->ssd = true;   /* whatever it asks, impose Server */
            int mh = wl_begin_msg(c, obj_id, 0 /* mode */);
            wl_push_u32(c, 2 /* Server */);
            wl_end_msg(c, mh);
            wl_client_flush(c);
        } else if (opcode == 0 /* release */) {
            if (ks) ks->ssd = false;
            wl_delete_obj(c, obj_id);
        }
        break;
    }

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
            wl_obj_t *so = wl_find_obj(c, surf_id);
            if (!so || so->type != OBJ_SURFACE) so = wl_find_obj_any(surf_id);
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

    /* ── wl_pointer ─────────────────────────────────────────────────── */
    case OBJ_POINTER:
        /* op=0: set_cursor, op=1: release/destroy — clear tracking ID */
        if (opcode == 1) {
            if (c->pointer_id == obj_id) c->pointer_id = 0;
            wl_delete_obj(c, obj_id);
        }
        /* set_cursor, motion, button etc silently accepted */
        break;

    /* ── wl_keyboard ─────────────────────────────────────────────────── */
    case OBJ_KEYBOARD:
        /* op=0: release/destroy — clear tracking ID */
        if (opcode == 0) {
            if (c->keyboard_id == obj_id) c->keyboard_id = 0;
            wl_delete_obj(c, obj_id);
        }
        break;

    default:
        /* Unknown/destroyed object — silently handle commit to unblock Firefox.
         * When closing a tab, Firefox commits to a surface we don't know about.
         * Without sending buffer_release + frame_done, Firefox hangs/crashes. */
        if (type == OBJ_NONE) {
            static uint32_t s_pending_buf = 0;
            if (opcode == WL_SURFACE_ATTACH && args_len >= 4) {
                /* Remember the buffer being attached to this unknown surface */
                memcpy(&s_pending_buf, args, 4);
            } else if (opcode == WL_SURFACE_COMMIT) {
                /* Send buffer_release so Firefox doesn't wait forever */
                if (s_pending_buf) {
                    wl_obj_t *bobj = wl_find_obj_any(s_pending_buf);
                    if (bobj) {
                        wl_client_t *bowner = NULL;
                        for (int _ci = 0; _ci < MAX_WL_CLIENTS; _ci++) {
                            if (wl_find_obj(&g_wl_clients[_ci], s_pending_buf)) {
                                bowner = &g_wl_clients[_ci]; break;
                            }
                        }
                        if (bowner && bowner->fd >= 0) {
                            int h = wl_begin_msg(bowner, s_pending_buf, WL_BUFFER_RELEASE);
                            wl_end_msg(bowner, h);
                            wl_client_flush(bowner);
                        }
                    }
                    s_pending_buf = 0;
                }
            } else if (opcode == WL_SURFACE_FRAME && args_len >= 4) {
                /* Fire frame callback immediately so rendering loop doesn't stall */
                uint32_t cb_id; memcpy(&cb_id, args, 4);
                wl_new_obj(c, cb_id, OBJ_CALLBACK, NULL);
                send_wl_callback_done(c, cb_id, g_global_serial++);
            }
        }
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
            /* Rescue buffers to the orphan pool (a parent process may still commit
             * a surface referencing a GPU-process buffer after that process exits).
             * orphan_save NULLs each saved buffer's slot data so it won't be freed. */
            orphan_save_buffers(c);
            /* Free EVERYTHING else and reset the slot. Previously we kept all objects
             * as "zombies" for ID reuse — but that left pools and other structs with
             * live pointers that got aliased/double-freed across Firefox's many
             * connect/disconnect cycles. A Wayland reconnect is a NEW connection with
             * a fresh object-ID space, so there is nothing legitimate to preserve. */
            for (int i = 0; i < c->n_objs; i++)
                free_obj_data(&c->objs[i]);   /* rescued buffers are data=NULL → no-op */
            c->n_objs = 0;
            c->compositor_id = c->shm_id = c->seat_id = 0;
            c->keyboard_id = c->pointer_id = c->output_id = c->xdg_wm_id = 0;
            close(c->fd);
            c->fd     = -1;
            c->active = false;
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

/* Find the topmost mapped Wayland surface that contains (mx, my) */
static bool g_wl_minimized;   /* real definition below (taskbar minimize) */

/* Composed screen origin of a surface: popups sit at popup_x/y; a subsurface
 * adds its offset to the parent's origin (recursively — so a menu's content
 * subsurface lands on its popup, not at screen 0,0); toplevels use x/y. This is
 * what lets popup-menu hit-testing/pointer routing match where the menu draws. */
static void surface_screen_origin(wl_surface_t *s, int32_t *ox, int32_t *oy) {
    int guard = 0;
    int32_t ax = 0, ay = 0;
    while (s && guard++ < 8) {
        if (s->is_popup)      { ax += s->popup_x; ay += s->popup_y; return (void)(*ox = ax, *oy = ay); }
        if (s->is_subsurface) {
            ax += s->sub_x; ay += s->sub_y;
            wl_obj_t *po = wl_find_obj_any(s->parent_surface_id);
            s = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
            continue;
        }
        ax += s->x; ay += s->y; break;
    }
    *ox = ax; *oy = ay;
}

/* True if s is a grabbing popup or a subsurface descending from one — such
 * surfaces get pointer input but NOT keyboard enter (kbd on a popup tree drives
 * Firefox a11y and can crash it). */
static bool surface_in_popup_tree(wl_surface_t *s) {
    int guard = 0;
    while (s && guard++ < 8) {
        if (s->is_popup) return true;
        if (!s->is_subsurface) return false;
        wl_obj_t *po = wl_find_obj_any(s->parent_surface_id);
        s = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
    }
    return false;
}

/* Resolve the surface that should RECEIVE input for a hit surface. GTK/GDK (and
 * Chromium) root a window's input on its xdg_toplevel; a subsurface created only
 * for rendering (Firefox web content) isn't wired to the toolkit's input, so a
 * click delivered there is ignored. Walk a non-popup subsurface up to its
 * toplevel ancestor and return that (with its object id). Popups keep their own
 * surface (they hold the seat grab). */
static wl_surface_t *surface_input_target(int ci, wl_surface_t *s, uint32_t in_id, uint32_t *out_id) {
    *out_id = in_id;
    if (!s || !s->is_subsurface) return s;   /* a toplevel or popup: use as-is */
    /* Walk a render subsurface up to its nearest xdg ancestor — a toplevel (web
     * content → the window) OR a popup (menu content → the popup surface). GTK
     * roots input on the xdg surface, not the render subsurface. */
    int guard = 0;
    while (s && s->is_subsurface && guard++ < 8) {
        uint32_t pid = s->parent_surface_id;
        wl_obj_t *po = wl_find_obj_any(pid);
        wl_surface_t *p = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
        if (!p) break;
        *out_id = pid;   /* parent_surface_id IS the parent's object id */
        s = p;
    }
    (void)ci;
    return s;
}

static bool wl_surface_hit(wl_surface_t *s, int32_t mx, int32_t my) {
    if (!s || !s->mapped || s->minimized || g_wl_minimized || !s->own_pix) return false;
    /* Composed screen origin for ALL surfaces: a subsurface's own s->x is 0 (its
     * real position is parent origin + sub_x/y), so hit-testing MUST use the
     * composed origin or clicks map to the wrong content coordinates. */
    int32_t ox, oy;
    surface_screen_origin(s, &ox, &oy);
    /* Hit-test against the ACTUAL committed buffer, not the configured size — a
     * client (e.g. Electron) may configure huge but commit a small window. */
    int32_t lx = mx - ox, ly = my - oy;
    if (lx < 0 || ly < 0 || lx >= s->own_w || ly >= s->own_h) return false;
    /* Click-through fully-transparent pixels: Electron apps create a transparent
     * full-screen "host" surface that would otherwise steal every click, and CSD
     * apps have transparent shadow margins. Only opaque pixels own the pointer. */
    uint32_t px = s->own_pix[(int64_t)ly * (int64_t)s->own_w + lx];
    return (px >> 24) != 0;
}

/* Wl-fixed is 24.8 fixed-point (value * 256) */
static uint32_t wl_fixed(int32_t v) { return (uint32_t)(v * 256); }

/* Monotonic millisecond timestamp for input events. Wall-clock seconds*1000
 * gave coarse, non-monotonic, often-duplicate stamps (motion and button sharing
 * one value) which Firefox/GTK can drop or mis-sequence. */
static uint32_t wl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

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
    /* Interactive move/resize: while active, the drag drives window geometry and
     * pointer events are NOT forwarded to the client. Ends when the button is up. */
    if (g_iop) {
        if (!(btns & 1)) { g_iop = 0; }   /* button released → finish */
        else if (g_iop_ci >= 0 && g_wl_clients[g_iop_ci].active) {
            wl_client_t *c = &g_wl_clients[g_iop_ci];
            wl_obj_t *o = wl_find_obj(c, g_iop_sid);
            wl_surface_t *s = (o && o->type == OBJ_SURFACE) ? o->data : NULL;
            if (s) {
                if (g_iop == 1) {                 /* move */
                    s->x = g_iop_ox + (mx - g_iop_sx);
                    s->y = g_iop_oy + (my - g_iop_sy);
                    if (s->ssd) {
                        extern uint32_t console_font_height(void);
                        int32_t top2 = (int32_t)console_font_height() + 6 + SSD_TITLE_H;
                        if (s->y < top2) s->y = top2;
                    }
                } else {                          /* resize */
                    int32_t nw = g_iop_ow, nh = g_iop_oh, nx = g_iop_ox, ny = g_iop_oy;
                    int32_t dx = mx - g_iop_sx, dy = my - g_iop_sy;
                    /* edges: 1=top 2=bottom 4=left 8=right */
                    if (g_iop_edges & 8) nw = g_iop_ow + dx;
                    if (g_iop_edges & 4) { nw = g_iop_ow - dx; nx = g_iop_ox + dx; }
                    if (g_iop_edges & 2) nh = g_iop_oh + dy;
                    if (g_iop_edges & 1) { nh = g_iop_oh - dy; ny = g_iop_oy + dy; }
                    if (nw < 200) nw = 200;
                    if (nh < 150) nh = 150;
                    s->x = nx; s->y = ny;
                    send_toplevel_configure(c, s, nw, nh,
                                            XDG_TOPLEVEL_STATE_RESIZING, 0);
                    wl_client_flush(c);
                }
            } else { g_iop = 0; }
        } else { g_iop = 0; }
        g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns;
        return;
    }
    /* SSD edge resize: press near the frame of a decorated toplevel starts an
     * interactive resize (edges: 1=top 2=bottom 4=left 8=right). */
    if ((btns & 1) && !(g_prev_btns & 1)) {
        wl_surface_t *rs = NULL; int rs_ci = -1; uint32_t rs_sid = 0;
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            wl_client_t *cc = &g_wl_clients[ci];
            if (!cc->active) continue;
            for (int oi = 0; oi < cc->n_objs; oi++) {
                if (cc->objs[oi].type != OBJ_SURFACE) continue;
                wl_surface_t *es = cc->objs[oi].data;
                if (g_wl_minimized || !ssd_decorated(es)) continue;
                const int32_t M = 6;
                int32_t wx0 = es->x, wy0 = es->y - SSD_TITLE_H;
                int32_t wx1 = es->x + es->own_w, wy1 = es->y + es->own_h;
                if (mx < wx0 - M || mx > wx1 + M || my < wy0 - M || my > wy1 + M) continue;
                uint32_t e = 0;
                if (mx <= wx0 + M) e |= 4;
                if (mx >= wx1 - M) e |= 8;
                if (my <= wy0 + M) e |= 1;
                if (my >= wy1 - M) e |= 2;
                if (e) { rs = es; rs_ci = ci; rs_sid = cc->objs[oi].id; g_iop_edges = (int)e; }
            }
        }
        if (rs) {
            g_iop = 2; g_iop_ci = rs_ci; g_iop_sid = rs_sid;
            g_iop_sx = mx; g_iop_sy = my;
            g_iop_ox = rs->x; g_iop_oy = rs->y;
            g_iop_ow = rs->own_w; g_iop_oh = rs->own_h;
            rs->maximized = false;    /* manual resize leaves maximized state */
            g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns;
            return;
        }
    }

    /* Server-side decoration bar: clicks on the FiFi titlebar belong to the
     * compositor (drag / minimize / maximize / close) and are never forwarded. */
    {
        int bar_ci = -1; uint32_t bar_sid = 0; wl_surface_t *bar_s = NULL;
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            wl_client_t *cc = &g_wl_clients[ci];
            if (!cc->active) continue;
            for (int oi = 0; oi < cc->n_objs; oi++) {
                if (cc->objs[oi].type != OBJ_SURFACE) continue;
                wl_surface_t *bs = cc->objs[oi].data;
                if (g_wl_minimized || !ssd_decorated(bs)) continue;
                if (mx >= bs->x && mx < bs->x + bs->own_w &&
                    my >= bs->y - SSD_TITLE_H && my < bs->y) {
                    bar_ci = ci; bar_sid = cc->objs[oi].id; bar_s = bs;
                }
            }
        }
        if (bar_s) {
            if ((btns & 1) && !(g_prev_btns & 1)) {
                int32_t rel = mx - bar_s->x;
                int32_t bw  = bar_s->own_w;
                wl_client_t *bc = &g_wl_clients[bar_ci];
                extern uint32_t console_font_height(void);
                int32_t fh2 = (int32_t)console_font_height();
                int32_t top = fh2 + 6 + SSD_TITLE_H;
                if (rel >= bw - 24) {                     /* close */
                    int h = wl_begin_msg(bc, bar_s->xdg_toplevel_id, XDG_TOPLEVEL_CLOSE);
                    wl_end_msg(bc, h);
                    wl_client_flush(bc);
                } else if (rel >= bw - 48) {              /* maximize / restore */
                    int32_t taskbar = fh2 + 10;
                    if (!bar_s->maximized) {
                        bar_s->restore_x = bar_s->x; bar_s->restore_y = bar_s->y;
                        bar_s->restore_w = bar_s->own_w; bar_s->restore_h = bar_s->own_h;
                        bar_s->maximized = true;
                        bar_s->x = 0; bar_s->y = top;
                        send_toplevel_configure(bc, bar_s, g_w, g_h - top - taskbar,
                                                XDG_TOPLEVEL_STATE_MAXIMIZED, 0);
                    } else {
                        bar_s->maximized = false;
                        bar_s->x = bar_s->restore_x;
                        bar_s->y = bar_s->restore_y < top ? top : bar_s->restore_y;
                        send_toplevel_configure(bc, bar_s, bar_s->restore_w, bar_s->restore_h, 0, 0);
                    }
                    wl_client_flush(bc);
                } else if (rel >= bw - 72) {              /* minimize (taskbar restores) */
                    g_wl_minimized = true;
                } else {                                   /* drag-move */
                    g_iop = 1; g_iop_ci = bar_ci; g_iop_sid = bar_sid;
                    g_iop_sx = mx; g_iop_sy = my;
                    g_iop_ox = bar_s->x; g_iop_oy = bar_s->y;
                }
            }
            g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns;
            return;
        }
    }

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
                /* Deliver input to the toolkit's real input surface: a non-popup
                 * render subsurface (web content) resolves up to its toplevel. */
                uint32_t tid;
                wl_surface_t *tgt = surface_input_target(ci, s, c->objs[oi].id, &tid);
                /* Focus toplevels, popups (menus), and non-xdg surfaces. Popups
                 * get POINTER focus so menu items are clickable; keyboard enter is
                 * gated separately (surface_in_popup_tree) so Firefox a11y is safe. */
                if (tgt && (tgt->xdg_toplevel_id || tgt->is_popup || !tgt->xdg_surface_id)) {
                    new_ci  = ci;
                    new_sid = tid;
                    new_s   = tgt;
                }
                /* keep searching — last (topmost draw order) wins */
            }
        }
    }

    bool new_in_popup = new_s && surface_in_popup_tree(new_s);

    /* ── Pointer focus (follows whatever surface is under the cursor) ── */
    if (new_ci != g_focus_ci || new_sid != g_focus_sid) {
        if (g_focus_ci >= 0 && g_focus_sid) {
            wl_client_t *oc = &g_wl_clients[g_focus_ci];
            if (oc->active) { wl_send_ptr_leave(oc, g_focus_sid); wl_client_flush(oc); }
        }
        if (new_ci >= 0 && new_sid) {
            wl_client_t *nc = &g_wl_clients[new_ci];
            int32_t sox, soy;
            surface_screen_origin(new_s, &sox, &soy);
            wl_send_ptr_enter(nc, new_sid, mx - sox, my - soy);
            wl_client_flush(nc);
        }
        g_focus_ci  = new_ci;
        g_focus_sid = new_sid;
    }

    /* ── Keyboard focus (follows ONLY real toplevels, never a popup/menu) ──
     * Moving the pointer onto a menu leaves the keyboard on the toplevel, so
     * Firefox doesn't see the window deactivate and keep its menu open. */
    if (new_ci >= 0 && new_sid && !new_in_popup) {
        if (new_ci != g_kbd_ci || new_sid != g_kbd_sid) {
            if (g_kbd_ci >= 0 && g_kbd_sid) {
                wl_client_t *ko = &g_wl_clients[g_kbd_ci];
                if (ko->active) { wl_send_kbd_leave(ko, g_kbd_sid); wl_client_flush(ko); }
            }
            wl_client_t *nc = &g_wl_clients[new_ci];
            wl_send_kbd_enter(nc, new_sid);
            wl_client_flush(nc);
            g_kbd_ci  = new_ci;
            g_kbd_sid = new_sid;
        }
    }

    if (g_focus_ci < 0 || !g_focus_sid) { g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns; return; }

    wl_client_t *fc = &g_wl_clients[g_focus_ci];
    if (!fc->active || !fc->pointer_id) { g_prev_mx = mx; g_prev_my = my; g_prev_btns = btns; return; }

    /* Dismiss grabbed popup on click outside its bounds */
    if (btns && !g_prev_btns) {
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            wl_client_t *pc = &g_wl_clients[ci];
            if (!pc->active) continue;
            for (int oi = 0; oi < pc->n_objs; oi++) {
                if (pc->objs[oi].type != OBJ_SURFACE || !pc->objs[oi].data) continue;
                wl_surface_t *ps = pc->objs[oi].data;
                if (!ps->is_popup || !ps->popup_has_grab || !ps->mapped) continue;
                if (mx < ps->popup_x || mx >= ps->popup_x + ps->w ||
                    my < ps->popup_y || my >= ps->popup_y + ps->h) {
                    int ph = wl_begin_msg(pc, ps->xdg_popup_id, XDG_POPUP_DONE);
                    wl_end_msg(pc, ph);
                    ps->mapped = false;
                    ps->popup_has_grab = false;
                    wl_client_flush(pc);
                }
            }
        }
    }

    /* Find focused surface to compute local coords */
    wl_surface_t *fs = NULL;
    for (int oi = 0; oi < fc->n_objs; oi++) {
        if (fc->objs[oi].id == g_focus_sid && fc->objs[oi].type == OBJ_SURFACE)
            fs = fc->objs[oi].data;
    }
    int32_t lx = mx, ly = my;
    if (fs) { int32_t fox, foy; surface_screen_origin(fs, &fox, &foy); lx = mx - fox; ly = my - foy; }

    /* Motion */
    if (mx != g_prev_mx || my != g_prev_my) {
        int h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_MOTION);
        wl_push_u32(fc, wl_now_ms());  /* time ms */
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
            wl_push_u32(fc, wl_now_ms());
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

/* Deliver a scroll-wheel event to the focused Wayland surface.
 * dir > 0 = wheel up, dir < 0 = wheel down. */
void wayland_send_scroll(int8_t dir) {
    if (!dir || g_focus_ci < 0 || !g_focus_sid) return;
    wl_client_t *fc = &g_wl_clients[g_focus_ci];
    if (!fc->active || !fc->pointer_id) return;
    /* wl_pointer.axis(time, axis=0 vertical, value). Positive value scrolls the
     * content down; wheel-up (dir>0) is a negative value. ~15 units per notch. */
    int h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_AXIS);
    wl_push_u32(fc, wl_now_ms());
    wl_push_u32(fc, 0);                       /* axis 0 = vertical scroll */
    wl_push_u32(fc, wl_fixed(dir > 0 ? -15 : 15));
    wl_end_msg(fc, h);
    h = wl_begin_msg(fc, fc->pointer_id, WL_PTR_FRAME);
    wl_end_msg(fc, h);
    wl_client_flush(fc);
}

/* Deliver a key event to the focused Wayland surface.
 * key is a Linux evdev keycode (KEY_A=30, etc.), state: 1=press, 0=release.
 * Tracks modifier keys so clients see correct shift/ctrl/alt state (xkb "us"
 * mod bits: Shift=1, Lock=2, Control=4, Mod1/Alt=8, Mod2/Num=16, Mod4/Super=64). */
void wayland_send_key(uint32_t evdev_key, uint32_t state) {
    static uint32_t s_depressed = 0;
    static uint32_t s_locked    = 0;
    uint32_t bit = 0;
    switch (evdev_key) {
        case 42: case 54:   bit = 1u;  break;   /* L/R shift  */
        case 29: case 97:   bit = 4u;  break;   /* L/R ctrl   */
        case 56: case 100:  bit = 8u;  break;   /* L/R alt    */
        case 125: case 126: bit = 64u; break;   /* L/R super  */
    }
    if (bit) { if (state) s_depressed |= bit; else s_depressed &= ~bit; }
    if (evdev_key == 58 && state) s_locked ^= 2u;    /* capslock toggles Lock */

    /* Keys go to the KEYBOARD-focused surface (a real toplevel), which stays put
     * while the pointer is over a menu — not the pointer-focused popup. */
    if (g_kbd_ci < 0 || !g_kbd_sid) return;
    wl_client_t *fc = &g_wl_clients[g_kbd_ci];
    if (!fc->active || !fc->keyboard_id) return;
    uint32_t ser = next_serial(fc);
    /* modifiers first so the key is interpreted with the current state */
    int h = wl_begin_msg(fc, fc->keyboard_id, WL_KBD_MODIFIERS);
    wl_push_u32(fc, ser);
    wl_push_u32(fc, s_depressed);
    wl_push_u32(fc, 0);          /* latched */
    wl_push_u32(fc, s_locked);
    wl_push_u32(fc, 0);          /* group */
    wl_end_msg(fc, h);
    h = wl_begin_msg(fc, fc->keyboard_id, WL_KBD_KEY);
    wl_push_u32(fc, next_serial(fc));
    wl_push_u32(fc, wl_now_ms());
    wl_push_u32(fc, evdev_key);
    wl_push_u32(fc, state);
    wl_end_msg(fc, h);
    wl_client_flush(fc);
}

/* Returns true if a Wayland surface has keyboard focus */
/* True only while the focused surface is still alive and visible. Without the
 * validation, focus goes stale when the window is closed or the layer is
 * minimized (wayland_send_mouse stops being called, so it never recomputes) and
 * every keystroke gets eaten instead of reaching IPC apps like the App Store. */
bool wayland_has_focus(void) {
    if (g_focus_ci < 0 || !g_focus_sid || g_wl_minimized) return false;
    wl_client_t *fc = &g_wl_clients[g_focus_ci];
    if (!fc->active) { g_focus_ci = -1; g_focus_sid = 0; return false; }
    wl_obj_t *o = wl_find_obj(fc, g_focus_sid);
    wl_surface_t *s = (o && o->type == OBJ_SURFACE) ? o->data : NULL;
    if (!s || !s->mapped || s->minimized || s->pending_destroy) {
        g_focus_ci = -1; g_focus_sid = 0;
        return false;
    }
    return true;
}

/* Whole-browser minimize state (taskbar-driven). When true the browser is hidden
 * and treated as absent for rendering/input, but still "present" for the taskbar. */
static bool g_wl_minimized = false;

/* True if a browser surface exists at all (mapped), regardless of minimized state. */
bool wayland_browser_present(void) {
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        if (!g_wl_clients[ci].active) continue;
        for (int oi = 0; oi < g_wl_clients[ci].n_objs; oi++) {
            if (g_wl_clients[ci].objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = g_wl_clients[ci].objs[oi].data;
            if (s && s->mapped && s->own_pix) return true;
        }
    }
    return false;
}
bool wayland_browser_minimized(void)      { return g_wl_minimized; }
void wayland_browser_set_minimized(bool m){ g_wl_minimized = m; }

/* Title of the topmost mapped Wayland toplevel (for the taskbar button).
 * Returns NULL when no client has set a title. */
const char *wayland_browser_title(void) {
    const char *best = NULL;
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        if (!g_wl_clients[ci].active) continue;
        for (int oi = 0; oi < g_wl_clients[ci].n_objs; oi++) {
            if (g_wl_clients[ci].objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = g_wl_clients[ci].objs[oi].data;
            if (s && s->mapped && s->own_pix && s->xdg_toplevel_id && s->title[0])
                best = s->title;
        }
    }
    return best;
}

/* Returns true if the browser is showing (mapped and not minimized). */
bool wayland_any_mapped(void) {
    if (g_wl_minimized) return false;
    return wayland_browser_present();
}

/* True if a mapped, non-minimized toplevel/subsurface (the browser window) contains
 * the screen point (x,y). Used for window z-order / input routing. */
bool wayland_covers(int32_t x, int32_t y) {
    if (g_wl_minimized) return false;
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        if (!g_wl_clients[ci].active) continue;
        for (int oi = 0; oi < g_wl_clients[ci].n_objs; oi++) {
            if (g_wl_clients[ci].objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = g_wl_clients[ci].objs[oi].data;
            if (!s || !s->mapped || !s->own_pix || s->minimized || s->is_popup) continue;
            int32_t ox = s->x, oy = s->y;
            if (s->is_subsurface) {
                wl_obj_t *po = wl_find_obj_any(s->parent_surface_id);
                wl_surface_t *p = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
                if (p) {
                    int32_t pox = p->is_popup ? p->popup_x : p->x;
                    int32_t poy = p->is_popup ? p->popup_y : p->y;
                    ox = pox + s->sub_x; oy = poy + s->sub_y;
                }
            }
            /* SSD toplevels: the FiFi titlebar sits ABOVE the surface (oy-SSD_TITLE_H
             * .. oy). Count it as covered so titlebar clicks route to the Wayland
             * layer (close/min/max/drag) regardless of what's painted underneath. */
            int32_t top_ext = ssd_decorated(s) ? SSD_TITLE_H : 0;
            if (x >= ox && x < ox + s->own_w && y >= oy - top_ext && y < oy + s->own_h) {
                if (y < oy) return true;          /* the SSD titlebar strip is opaque */
                int32_t lx = x - ox, ly = y - oy; /* else require an opaque pixel */
                if (s->own_pix &&
                    (s->own_pix[(int64_t)ly * (int64_t)s->own_w + lx] >> 24) != 0)
                    return true;
            }
        }
    }
    return false;
}

/* Drop keyboard focus from the Wayland layer (click-to-focus: called when the
 * user clicks a built-in/IPC window so keys stop being stolen from e.g. the App
 * Store while a Wayland app stays open behind it). */
void wayland_clear_kbd_focus(void) {
    if (g_kbd_ci >= 0 && g_kbd_sid) {
        wl_client_t *ko = &g_wl_clients[g_kbd_ci];
        if (ko->active) { wl_send_kbd_leave(ko, g_kbd_sid); wl_client_flush(ko); }
    }
    g_kbd_ci = -1; g_kbd_sid = 0;
    g_focus_ci = -1; g_focus_sid = 0;
}

/* Force-close the frontmost Wayland toplevel — a reliable close that works no
 * matter the focus/z-order state (titlebar-button close can get flaky with
 * Electron's multi-surface apps). Sends xdg_toplevel.close to every visible,
 * opaque toplevel of the most-recently-mapped client. Returns true if it sent. */
bool wayland_close_active(void) {
    bool sent = false;
    for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
        wl_client_t *c = &g_wl_clients[ci];
        if (!c->active) continue;
        for (int oi = 0; oi < c->n_objs; oi++) {
            if (c->objs[oi].type != OBJ_SURFACE) continue;
            wl_surface_t *s = c->objs[oi].data;
            if (!s || !s->xdg_toplevel_id || s->is_popup || s->is_subsurface) continue;
            if (!s->mapped) continue;
            int h = wl_begin_msg(c, s->xdg_toplevel_id, XDG_TOPLEVEL_CLOSE);
            wl_end_msg(c, h);
            wl_client_flush(c);
            sent = true;
        }
    }
    return sent;
}

/* Super+arrow snapping for the focused Wayland client's toplevel.
 * zone: 0=restore 1=left-half 2=right-half 3=maximize.
 * Returns false when no usable focused toplevel (caller tries other layers).
 *
 * Target selection: the KEYBOARD-focused client wins — it is sticky (last
 * toplevel the user was in) and is cleared when a built-in/IPC window is
 * clicked, so snapping works with the pointer anywhere on screen. Pointer
 * focus is the fallback. Within the client, prefer the ssd_decorated()
 * toplevel: Electron apps keep a phantom transparent full-screen host
 * surface that must never be the snap target. */
bool wayland_snap_focused(int zone) {
    if (g_wl_minimized) return false;
    int ci = -1;
    if (g_kbd_ci >= 0 && g_wl_clients[g_kbd_ci].active)        ci = g_kbd_ci;
    else if (g_focus_ci >= 0 && g_wl_clients[g_focus_ci].active) ci = g_focus_ci;
    if (ci < 0) return false;
    wl_client_t *c = &g_wl_clients[ci];
    wl_surface_t *s = NULL, *first = NULL;
    for (int oi = 0; oi < c->n_objs; oi++) {
        if (c->objs[oi].type != OBJ_SURFACE) continue;
        wl_surface_t *t = c->objs[oi].data;
        if (!t || !t->xdg_toplevel_id || t->is_popup || t->is_subsurface || !t->mapped) continue;
        if (!first) first = t;
        if (ssd_decorated(t)) { s = t; break; }
    }
    if (!s) s = first;
    if (!s || s->fullscreen) return false;
    extern uint32_t console_font_height(void);
    int32_t fh      = (int32_t)console_font_height();
    int32_t top     = fh + 6 + SSD_TITLE_H;
    int32_t taskbar = fh + 10;
    int32_t avail_h = g_h - top - taskbar;
    if (zone == 0) {
        if (s->maximized || s->half_snapped) {
            s->maximized = false; s->half_snapped = false;
            s->x = s->restore_x;
            s->y = s->restore_y < top ? top : s->restore_y;
            send_toplevel_configure(c, s, s->restore_w, s->restore_h, 0, 0);
            wl_client_flush(c);
        }
        return true;
    }
    if (!s->maximized && !s->half_snapped) {
        s->restore_x = s->x;     s->restore_y = s->y;
        s->restore_w = s->own_w; s->restore_h = s->own_h;
    }
    if (zone == 3) {
        s->maximized = true; s->half_snapped = false;
        s->x = 0; s->y = top;
        send_toplevel_configure(c, s, g_w, avail_h, XDG_TOPLEVEL_STATE_MAXIMIZED, 0);
    } else {
        /* halves are sent with the MAXIMIZED state too: toolkits honor
         * maximized configure sizes EXACTLY, while Electron ignores plain
         * floating sizes entirely. Compositor-side we track it as
         * half_snapped, not maximized. */
        s->maximized = false; s->half_snapped = true;
        s->x = (zone == 1) ? 0 : g_w / 2;
        s->y = top;
        send_toplevel_configure(c, s, (zone == 1) ? g_w / 2 : g_w - g_w / 2, avail_h,
                                XDG_TOPLEVEL_STATE_MAXIMIZED, 0);
    }
    wl_client_flush(c);
    return true;
}

/* ── Blit Wayland surfaces to the FiFi framebuffer ───────────────────────── */

/* FiFi Breeze chrome for server-side-decorated Wayland toplevels: identical
 * gradient titlebar + traffic-light circles as built-in and IPC windows. */
static void ssd_circle(uint64_t cx, uint64_t cy, uint32_t col) {
    extern void console_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c);
    static const uint8_t spans[13] = {1,7,9,11,11,11,13,11,11,11,9,7,1};
    for (int dy = -6; dy <= 6; dy++) {
        uint8_t sw = spans[dy + 6];
        console_fill_rect(cx - sw / 2u, cy + (uint64_t)(int64_t)dy, sw, 1u, col);
    }
}

/* Decode a UTF-8 title into ASCII so the bitmap font never renders raw
 * multibyte bytes as mojibake (Firefox uses an em-dash: "Page — LibreWolf").
 * Mirrors taskbar_ascii_label() in gui_taskbar.c. */
static void ssd_title_ascii(const char *in, char *out, size_t n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < n) {
        unsigned char c = *p;
        if (c < 0x80u) { out[o++] = (char)c; p++; continue; }
        uint32_t cp = 0; int cont = 0;
        if      ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; cont = 1; }
        else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; cont = 2; }
        else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; cont = 3; }
        else { p++; continue; }
        p++;
        for (int i = 0; i < cont && (*p & 0xC0u) == 0x80u; i++) { cp = (cp << 6) | (*p & 0x3Fu); p++; }
        char r;
        switch (cp) {
            case 0x2012: case 0x2013: case 0x2014: case 0x2015: r = '-'; break;
            case 0x2018: case 0x2019: case 0x201A: case 0x2032: r = '\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x2033: r = '"'; break;
            case 0x2026: r = '.'; break;
            case 0x00B7: case 0x2022: case 0x2027: r = '-'; break;
            case 0x00A0: case 0x2009: case 0x202F: r = ' '; break;
            default: r = (cp < 0x80u) ? (char)cp : '?'; break;
        }
        out[o++] = r;
    }
    out[o] = '\0';
}

static void ssd_draw_chrome(int ci, wl_surface_t *s, int32_t bx, int32_t by) {
    extern void console_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c);
    extern void console_fill_vgrad(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t c0, uint32_t c1);
    extern void console_render_glyph_fg(uint64_t px, uint64_t py, unsigned char ch, uint32_t fg);
    extern uint32_t console_font_width(void);
    extern uint32_t console_font_height(void);
    if (by < SSD_TITLE_H) return;
    bool focused = (ci == g_focus_ci);
    uint64_t x = (uint64_t)bx, w = (uint64_t)s->own_w;
    uint64_t ty = (uint64_t)(by - SSD_TITLE_H);
    uint32_t grad_top = focused ? 0x00324a72u : 0x00202836u;
    uint32_t grad_bot = focused ? 0x001e2c48u : 0x00161c26u;
    console_fill_vgrad(x, ty, w, SSD_TITLE_H, grad_top, grad_bot);
    console_fill_rect(x, ty, w, 1u, focused ? 0x00466492u : 0x002a3446u);
    console_fill_rect(x, ty + SSD_TITLE_H - 1u, w, 1u, 0x0010192au);
    /* title text, centered */
    uint64_t fw = console_font_width(), fh = console_font_height();
    if (fw && fh && s->title[0]) {
        char tbuf[128];
        ssd_title_ascii(s->title, tbuf, sizeof tbuf);
        /* Apps title as "<page/doc> - <App>" (browsers, editors); show just the
         * app name — keep only the segment after the last " - " separator. */
        const char *ttl = tbuf;
        for (size_t j = 1; tbuf[j] && tbuf[j + 1]; j++)
            if (tbuf[j] == '-' && tbuf[j - 1] == ' ' && tbuf[j + 1] == ' ')
                ttl = &tbuf[j + 2];
        size_t tlen = 0; while (ttl[tlen]) tlen++;
        uint64_t max_px = w > 84u ? w - 84u : 0u;   /* keep clear of buttons */
        if (tlen * fw > max_px) tlen = max_px / fw;
        uint64_t tx = x + (w > tlen * fw ? (w - tlen * fw) / 2u : 8u);
        uint64_t gy = ty + (SSD_TITLE_H > fh ? (SSD_TITLE_H - fh) / 2u : 0u);
        for (size_t j = 0; j < tlen; j++, tx += fw)
            console_render_glyph_fg(tx, gy, (unsigned char)ttl[j], 0x00e8eeffu);
    }
    /* traffic lights: min grey, max green, close red */
    uint64_t cyc = ty + SSD_TITLE_H / 2u;
    if (w >= 88u) {
        ssd_circle(x + w - 60u, cyc, 0x00707a8cu);
        ssd_circle(x + w - 36u, cyc, 0x005e9e56u);
    }
    ssd_circle(x + w - 12u, cyc, 0x00c05048u);
    /* frame + focus ring around bar+content */
    uint64_t th = (uint64_t)SSD_TITLE_H + (uint64_t)s->own_h;
    uint32_t frame = focused ? 0x00283a58u : 0x001d2634u;
    console_fill_rect(x,          ty,          w, 1u, frame);
    console_fill_rect(x,          ty + th - 1, w, 1u, frame);
    console_fill_rect(x,          ty,          1u, th, frame);
    console_fill_rect(x + w - 1,  ty,          1u, th, frame);
    if (focused) {
        uint32_t ring = 0x002b4d80u;
        if (ty > 0)   console_fill_rect(x > 0 ? x-1 : 0, ty - 1, w + 2, 1u, ring);
        console_fill_rect(x > 0 ? x-1 : 0, ty + th, w + 2, 1u, ring);
        if (x > 0)    console_fill_rect(x - 1, ty, 1u, th, ring);
        console_fill_rect(x + w, ty, 1u, th, ring);
    }
}

/* Called from compositor main after ipc_blit_all() */
/* Blit one surface at its computed screen position */
static int blit_one_surface(int ci, wl_surface_t *s, uint32_t obj_id, int do_log) {
    extern void console_paste_rect(const uint32_t *src, uint64_t dx, uint64_t dy,
                                    uint64_t w, uint64_t h);
    extern void console_paste_rect_alpha(const uint32_t *src, uint64_t dx, uint64_t dy,
                                    uint64_t w, uint64_t h);
    (void)ci; (void)obj_id; (void)do_log;
    /* Blit from our OWN packed copy (own_w*own_h, tightly packed). Never touches
     * client buffer memory, so it can't fault on a freed/resized client buffer. */
    if (!s->mapped || !s->own_pix || s->minimized) return 0;
    if (s->own_w <= 0 || s->own_h <= 0) return 0;
    if (s->is_popup) {
        /* Only draw popups that took an input grab — i.e. interactive menus and
         * dropdowns. Passive popups (hover tooltips) are skipped: their multi-surface
         * layout composites poorly here (black frames, flicker, fragments) and they
         * add no function. This keeps right-click menus / dropdowns working. */
        if (!s->popup_has_grab) return 0;
        int total = s->own_w * s->own_h, step = total > 4096 ? total / 4096 : 1, opaque = 0;
        for (int i = 0; i < total; i += step) if ((s->own_pix[i] >> 24) != 0) { opaque = 1; break; }
        if (!opaque) return 0;
    }
    int32_t bx, by;
    if (s->is_popup) {
        bx = s->popup_x; by = s->popup_y;
    } else if (s->is_subsurface) {
        wl_obj_t *po = wl_find_obj_any(s->parent_surface_id);
        wl_surface_t *p = (po && po->type == OBJ_SURFACE) ? po->data : NULL;
        if (!p) return 0;
        /* Parent origin: a popup parent (e.g. a menu) is positioned via popup_x/y,
         * NOT x/y (which stays 0). Firefox renders menu content as a subsurface of
         * the popup — using p->x here draws it at screen-left instead of at the
         * menu. Honor the popup position so the content follows its popup frame. */
        int32_t pox = p->is_popup ? p->popup_x : p->x;
        int32_t poy = p->is_popup ? p->popup_y : p->y;
        bx = pox + s->sub_x;
        by = poy + s->sub_y;
    } else {
        bx = s->x; by = s->y;
    }
    /* Subsurface = the browser content: draw only fully-opaque pixels so the ENTIRE
     * CSD shadow (transparent margin + semi-transparent gradient) is skipped — no
     * shadow at all. Popups (menus) may be legitimately semi-transparent, so they
     * only skip fully-transparent pixels. Toplevels are the opaque base — plain copy. */
    extern void console_paste_rect_opaque(const uint32_t *src, uint64_t dx, uint64_t dy,
                                          uint64_t w, uint64_t h);
    extern void console_paste_rect_blend(const uint32_t *src, uint64_t dx, uint64_t dy,
                                         uint64_t w, uint64_t h);
    /* All Wayland surfaces are ARGB. Alpha-blend over the wallpaper (repainted under
     * the window each frame) so the browser's semi-transparent CSD shadow margin
     * composites away instead of being stamped as a black ring. */
    console_paste_rect_blend(s->own_pix, (uint64_t)bx, (uint64_t)by,
                             (uint64_t)s->own_w, (uint64_t)s->own_h);
    if (ssd_decorated(s))
        ssd_draw_chrome(ci, s, bx, by);
    return 1;
}

void wayland_blit_surfaces(void) {
    if (g_wl_minimized) return;   /* browser minimized — draw nothing */
    static int blit_log_ticker = 0;
    int do_log = (++blit_log_ticker % 600 == 0);
    int blitted = 0;

    /* Three-pass: non-subsurface non-popup first, then subsurfaces, then popups on top. */
    for (int pass = 0; pass < 3; pass++) {
        for (int ci = 0; ci < MAX_WL_CLIENTS; ci++) {
            wl_client_t *c = &g_wl_clients[ci];
            if (!c->active) continue;
            for (int oi = 0; oi < c->n_objs; oi++) {
                if (c->objs[oi].type != OBJ_SURFACE) continue;
                wl_surface_t *s = c->objs[oi].data;
                if (!s) continue;
                /* pass 0 = non-subsurfaces, pass 1 = subsurfaces */
                if (pass == 0 && (s->is_subsurface || s->is_popup)) continue;
                if (pass == 1 && (!s->is_subsurface || s->is_popup)) continue;
                if (pass == 2 && !s->is_popup) continue;
                blitted += blit_one_surface(ci, s, c->objs[oi].id, do_log);
            }
        }
    }
    (void)blitted;
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
    /* 0666: user-namespaced clients (archimage apps run with a fake non-root
     * uid) must still be able to connect. The socket dir remains protected. */
    chmod(g_sock_path, 0666);

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
