/* ── xwm.c — minimal X11 window manager for rootless XWayland ────────────────
 * See xwm.h. Hand-rolls just enough of the X11 wire protocol (little-endian,
 * x86-64) to: connect to XWayland's X server, take over window management
 * (SubstructureRedirect on the root), and for each X toplevel learn its
 * WL_SURFACE_ID so wayland.c can present the Wayland surface XWayland created
 * as an ordinary FiFi window. No XCB/Xlib — the compositor is a static binary. */

#define _GNU_SOURCE
#include "xwm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

/* ── Wayland-side hooks (implemented in wayland.c) ───────────────────────────
 * wayland_x11_adopt() is declared in xwm.h (keyed on the xwayland_shell_v1
 * serial). Unmap/geometry/title keep the compositor's view in sync. */
void wayland_x11_unmap(uint32_t xwindow);
void wayland_x11_geometry(uint32_t xwindow, int32_t x, int32_t y,
                          int32_t w, int32_t h);
void wayland_x11_title(uint32_t xwindow, const char *title);

/* ── X11 protocol opcodes / events ───────────────────────────────────────── */
enum { X_ChangeWindowAttributes = 2, X_GetWindowAttributes = 3,
       X_MapWindow = 8, X_ConfigureWindow = 12, X_GetGeometry = 14,
       X_QueryTree = 15, X_InternAtom = 16, X_ChangeProperty = 18,
       X_GetProperty = 20, X_SendEvent = 25, X_GrabServer = 36,
       X_UngrabServer = 37, X_SetInputFocus = 42, X_KillClient = 113 };

enum { XE_Error = 0, XE_Reply = 1, XE_CreateNotify = 16, XE_DestroyNotify = 17,
       XE_UnmapNotify = 18, XE_MapNotify = 19, XE_MapRequest = 20,
       XE_ReparentNotify = 21, XE_ConfigureNotify = 22, XE_ConfigureRequest = 23,
       XE_PropertyNotify = 28, XE_ClientMessage = 33 };

/* ChangeWindowAttributes / ConfigureWindow value-mask bits */
#define CW_EVENT_MASK      0x0800u
#define EV_SUBSTRUCT_REDIR 0x00100000u
#define EV_SUBSTRUCT_NOTIF 0x00080000u
#define EV_PROPERTY_CHANGE 0x00400000u
#define CFG_X 0x01u
#define CFG_Y 0x02u
#define CFG_W 0x04u
#define CFG_H 0x08u
#define CFG_STACK 0x40u

/* ── Managed-window table ─────────────────────────────────────────────────── */
#define XWM_MAX_WINS 64
typedef struct {
    uint32_t window;         /* X window id (0 = free slot) */
    int32_t  x, y, w, h;
    bool     override_redirect;
    bool     mapped;
    uint64_t serial;         /* WL_SURFACE_SERIAL (0 = not read yet) */
    bool     serial_known;
    bool     adopted;        /* handed to wayland.c */
    char     title[128];
} xwin_t;

/* ── Connection state ─────────────────────────────────────────────────────── */
typedef struct {
    int      fd;
    bool     up;
    uint32_t root;
    uint32_t id_base, id_mask;
    uint32_t next_id;        /* rolling XID counter within the id range */
    uint32_t seq;            /* last request sequence number sent */
    /* interned atoms */
    uint32_t a_wl_surface_id, a_wl_surface_serial, a_wm_protocols, a_wm_delete,
             a_net_wm_name, a_wm_name, a_utf8, a_net_active, a_net_wm_state,
             a_net_wm_state_fs, a_wm_state, a_net_supported;
    /* async GetProperty round-trips: seq -> (window, which property) */
    struct { uint32_t seq, window, atom; } prop_pend[XWM_MAX_WINS];
    int      n_prop_pend;
    /* incoming byte accumulator */
    uint8_t  rbuf[16384];
    int      rused;
    xwin_t   wins[XWM_MAX_WINS];
} xwm_t;

static xwm_t X;

/* ── Little-endian pack helpers ───────────────────────────────────────────── */
static inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* Blocking write of the whole buffer (requests are small; the X socket rarely
 * blocks for a WM). Returns false on a hard error. */
static bool x_write(const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(X.fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        return false;
    }
    return true;
}

/* Every request bumps the server's sequence counter. */
static uint32_t x_bump_seq(void) { return ++X.seq; }

static uint32_t x_alloc_id(void) {
    /* XIDs are id_base | (n & id_mask); wrap within the granted range. */
    uint32_t id = X.id_base | (X.next_id & X.id_mask);
    X.next_id++;
    return id;
}

/* ── Requests ─────────────────────────────────────────────────────────────── */
static void x_change_event_mask(uint32_t window, uint32_t mask) {
    uint8_t r[16];
    r[0] = X_ChangeWindowAttributes; r[1] = 0;
    put16(r + 2, 4);                 /* request length in 4-byte units */
    put32(r + 4, window);
    put32(r + 8, CW_EVENT_MASK);
    put32(r + 12, mask);
    x_bump_seq(); x_write(r, sizeof r);
}

static void x_map_window(uint32_t window) {
    uint8_t r[8];
    r[0] = X_MapWindow; r[1] = 0; put16(r + 2, 2);
    put32(r + 4, window);
    x_bump_seq(); x_write(r, sizeof r);
}

/* ConfigureWindow with x/y/w/h + raise-to-top. w/h<=0 are omitted. */
static void x_configure(uint32_t window, int32_t x, int32_t y, int32_t w, int32_t h, bool raise) {
    uint8_t r[8 + 5 * 4];
    uint32_t mask = CFG_X | CFG_Y;
    int vi = 0; uint8_t vals[5 * 4];
    put32(vals + vi, (uint32_t)x); vi += 4;
    put32(vals + vi, (uint32_t)y); vi += 4;
    if (w > 0) { mask |= CFG_W; put32(vals + vi, (uint32_t)w); vi += 4; }
    if (h > 0) { mask |= CFG_H; put32(vals + vi, (uint32_t)h); vi += 4; }
    if (raise) { mask |= CFG_STACK; put32(vals + vi, 0 /*Above*/); vi += 4; }
    int nval = vi / 4;
    r[0] = X_ConfigureWindow; r[1] = 0;
    put16(r + 2, (uint16_t)(3 + nval));
    put32(r + 4, window);
    put16(r + 8, (uint16_t)mask);
    put16(r + 10, 0);
    memcpy(r + 12, vals, (size_t)vi);
    x_bump_seq(); x_write(r, (size_t)(12 + vi));
}

static void x_set_input_focus(uint32_t window) {
    uint8_t r[12];
    r[0] = X_SetInputFocus; r[1] = 2 /*RevertToParent*/; put16(r + 2, 3);
    put32(r + 4, window);
    put32(r + 8, 0 /*CurrentTime*/);
    x_bump_seq(); x_write(r, sizeof r);
}

/* async InternAtom; caller reads the reply during the synchronous init phase. */
static void x_intern_atom(const char *name) {
    uint16_t nl = (uint16_t)strlen(name);
    uint16_t pad = (uint16_t)((4 - (nl & 3)) & 3);
    uint8_t r[8 + 256];
    if (nl > 240) return;
    r[0] = X_InternAtom; r[1] = 0 /*only-if-exists=false*/;
    put16(r + 2, (uint16_t)(2 + (nl + pad) / 4));
    put16(r + 4, nl);
    put16(r + 6, 0);
    memcpy(r + 8, name, nl);
    memset(r + 8 + nl, 0, pad);
    x_bump_seq(); x_write(r, (size_t)(8 + nl + pad));
}

/* async GetProperty; reply matched by sequence later (window + which atom). */
static void x_get_property(uint32_t window, uint32_t prop) {
    uint8_t r[24];
    r[0] = X_GetProperty; r[1] = 0 /*delete=false*/; put16(r + 2, 6);
    put32(r + 4, window);
    put32(r + 8, prop);
    put32(r + 12, 0 /*AnyPropertyType*/);
    put32(r + 16, 0 /*long-offset*/);
    put32(r + 20, 64 /*long-length: up to 256 bytes*/);
    uint32_t seq = x_bump_seq();
    x_write(r, sizeof r);
    if (X.n_prop_pend < XWM_MAX_WINS) {
        X.prop_pend[X.n_prop_pend].seq = seq;
        X.prop_pend[X.n_prop_pend].window = window;
        X.prop_pend[X.n_prop_pend].atom = prop;
        X.n_prop_pend++;
    }
}

/* ChangeProperty(Replace) of `n` CARD32 values (format 32). */
static void x_change_prop32(uint32_t window, uint32_t prop, uint32_t type,
                            const uint32_t *vals, int n) {
    uint8_t r[24 + 32 * 4];
    if (n > 32) n = 32;
    r[0] = X_ChangeProperty; r[1] = 0 /*Replace*/;
    put16(r + 2, (uint16_t)(6 + n));      /* length in 4-byte units */
    put32(r + 4, window);
    put32(r + 8, prop);
    put32(r + 12, type);
    r[16] = 32; r[17] = r[18] = r[19] = 0;
    put32(r + 20, (uint32_t)n);
    for (int i = 0; i < n; i++) put32(r + 24 + i * 4, vals[i]);
    x_bump_seq(); x_write(r, (size_t)(24 + n * 4));
}

/* WM_STATE (ICCCM): mark a managed window Normal (1). GTK and many toolkits gate
 * their "mapped, start painting" logic on seeing WM_STATE set by the WM. */
static void x_set_wm_state(uint32_t window, uint32_t state) {
    if (!X.a_wm_state) return;
    uint32_t v[2] = { state, 0 /*icon window = None*/ };
    x_change_prop32(window, X.a_wm_state, X.a_wm_state, v, 2);
}

/* Synthetic ConfigureNotify (ICCCM 4.2.3): after a WM maps/positions a window it
 * must tell the client its final geometry, or some toolkits never finish layout
 * and never paint. Sent to the window with StructureNotify mask. */
static void x_send_configure_notify(uint32_t window, int32_t x, int32_t y,
                                    int32_t w, int32_t h) {
    uint8_t r[44];
    memset(r, 0, sizeof r);
    r[0] = X_SendEvent; r[1] = 0 /*propagate*/; put16(r + 2, 11);
    put32(r + 4, window);              /* destination */
    put32(r + 8, 0x00020000u);         /* event-mask = StructureNotify */
    uint8_t *e = r + 12;               /* 32-byte ConfigureNotify body */
    e[0] = XE_ConfigureNotify;
    put32(e + 4, window);              /* event window */
    put32(e + 8, window);              /* the window */
    put32(e + 12, 0);                  /* above-sibling = None */
    put16(e + 16, (uint16_t)x);
    put16(e + 18, (uint16_t)y);
    put16(e + 20, (uint16_t)w);
    put16(e + 22, (uint16_t)h);
    put16(e + 24, 0);                  /* border-width */
    e[26] = 0;                         /* override-redirect */
    x_bump_seq(); x_write(r, sizeof r);
}

/* Polite close: ClientMessage WM_PROTOCOLS/WM_DELETE_WINDOW via SendEvent. */
static void x_send_delete(uint32_t window) {
    uint8_t r[44];
    memset(r, 0, sizeof r);
    r[0] = X_SendEvent; r[1] = 0 /*propagate*/; put16(r + 2, 11);
    put32(r + 4, window);            /* destination */
    put32(r + 8, 0);                 /* event-mask = 0 (deliver to window) */
    /* 32-byte event body at offset 12 */
    uint8_t *e = r + 12;
    e[0] = XE_ClientMessage; e[1] = 32 /*format*/;
    put16(e + 2, 0);
    put32(e + 4, window);
    put32(e + 8, X.a_wm_protocols);
    put32(e + 12, X.a_wm_delete);
    put32(e + 16, 0 /*CurrentTime*/);
    x_bump_seq(); x_write(r, sizeof r);
}

/* ── Managed-window table ops ─────────────────────────────────────────────── */
static xwin_t *xwin_find(uint32_t window) {
    for (int i = 0; i < XWM_MAX_WINS; i++)
        if (X.wins[i].window == window) return &X.wins[i];
    return NULL;
}
static xwin_t *xwin_add(uint32_t window) {
    xwin_t *w = xwin_find(window);
    if (w) return w;
    for (int i = 0; i < XWM_MAX_WINS; i++)
        if (X.wins[i].window == 0) {
            memset(&X.wins[i], 0, sizeof(xwin_t));
            X.wins[i].window = window;
            return &X.wins[i];
        }
    return NULL;
}
static void xwin_remove(uint32_t window) {
    xwin_t *w = xwin_find(window);
    if (w) memset(w, 0, sizeof(xwin_t));
}

/* Adopt once we have BOTH the serial (WL_SURFACE_SERIAL) and the window mapped.
 * The Wayland set_serial may lag the X property, so adopt() can return false;
 * we leave the window pending and retry from xwm_poll. */
static void xwin_try_adopt(xwin_t *w) {
    if (!w || w->adopted || !w->serial_known || !w->mapped) return;
    if (!wayland_x11_adopt(w->serial, w->window, w->x, w->y, w->w, w->h,
                           !w->override_redirect,
                           w->title[0] ? w->title : "X11 Window"))
        return;                  /* surface not ready yet — retry next poll */
    w->adopted = true;
    /* Fetch the real title asynchronously (prefer _NET_WM_NAME, then WM_NAME). */
    if (!w->override_redirect) {
        if (X.a_net_wm_name) x_get_property(w->window, X.a_net_wm_name);
        if (X.a_wm_name)      x_get_property(w->window, X.a_wm_name);
    }
}

/* ── XWayland lifecycle ───────────────────────────────────────────────────── */
static pid_t s_xwl_pid = -1;
static bool  s_setup_sent = false;
static int   s_screen_w = 0, s_screen_h = 0;   /* rootful X screen size */

/* The compositor's framebuffer + work-area geometry (from console/gui). Used to
 * size the rootful X screen so a maximized app fills the usable desktop. */
extern uint32_t console_fb_width(void);
extern uint32_t console_fb_height(void);

/* Spawn XWayland with -wm on one end of a socketpair; the WM uses the other end
 * as its X connection. -wm tells XWayland who the window manager is (and implies
 * rootless) — that is what makes it tag each window's wl_surface via
 * xwayland_shell_v1 so we can correlate + present it. Returns the WM-side fd. */
static int xwl_spawn(void) {
    /* Size the rootful X screen to (nearly) the display; a maximized app then
     * fills it, leaving no black X-root border. Leave margins for the FiFi
     * titlebar + taskbar so the whole window stays on the work area. */
    uint32_t fw = console_fb_width(), fh = console_fb_height();
    s_screen_w = (int)fw > 200 ? (int)fw - 160 : (int)fw;
    s_screen_h = (int)fh > 220 ? (int)fh - 200 : (int)fh;
    if (s_screen_w < 640) s_screen_w = 640;
    if (s_screen_h < 480) s_screen_h = 480;
    unlink("/tmp/.X11-unix/X0");            /* clear any stale display */
    unlink("/tmp/.X0-lock");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (pid == 0) {
        close(sv[0]);
        fcntl(sv[1], F_SETFD, 0);           /* clear CLOEXEC so XWayland inherits it */
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        setenv("LD_LIBRARY_PATH", "/fifi-data/runtime/lib", 1);
        setenv("PATH", "/fifi-data/runtime/bin:/usr/bin:/bin", 1);
        /* /usr resets each boot: XWayland execs the compiled-in /usr/bin/xkbcomp
         * and client toolkits read /usr/share/X11/xkb, so seed both from the
         * persistent runtime or XWayland aborts ("Failed to activate keyboard"). */
        if (access("/usr/bin/xkbcomp", X_OK) != 0)
            symlink("/fifi-data/runtime/bin/xkbcomp", "/usr/bin/xkbcomp");
        if (access("/usr/share/X11/xkb", F_OK) != 0) {
            mkdir("/usr/share", 0755); mkdir("/usr/share/X11", 0755);
            symlink("/fifi-data/runtime/share/X11/xkb", "/usr/share/X11/xkb");
        }
        char fds[16]; snprintf(fds, sizeof fds, "%d", sv[1]);
        char geo[32]; snprintf(geo, sizeof geo, "%dx%d", s_screen_w, s_screen_h);
        /* Rootful (NOT -rootless): rootless presentation does not composite
         * per-window on this hand-rolled compositor, but rootful + -wm renders
         * fine and the WM maximizes the app to fill the screen (no black
         * border). -wm gives XWayland our WM connection. */
        execl("/usr/bin/Xwayland", "Xwayland", ":0", "-wm", fds,
              "-geometry", geo,
              "-xkbdir", "/fifi-data/runtime/share/X11/xkb", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    s_xwl_pid = pid;
    struct timeval tv = { 0, 800000 };      /* bound blocking handshake reads */
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    fprintf(stderr, "[xwm] spawned XWayland pid=%d (-wm fd)\n", (int)pid);
    return sv[0];
}

/* Read exactly n bytes (blocking) during the synchronous handshake. */
static bool x_read_full(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(X.fd, buf + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

/* Send the X11 connection-setup request (empty auth — the -wm fd is already an
 * authenticated connection). XWayland replies once it has started. */
static bool x_send_setup(void) {
    uint8_t req[12];
    memset(req, 0, sizeof req);
    req[0] = 'l';               /* byte-order: little-endian */
    put16(req + 2, 11);         /* protocol-major */
    put16(req + 4, 0);          /* protocol-minor */
    put16(req + 6, 0);          /* auth-proto-name length */
    put16(req + 8, 0);          /* auth-proto-data length */
    return x_write(req, sizeof req);
}

/* Read + parse the setup reply (root window + resource-id base/mask). Called
 * once the -wm fd is readable, so the blocking reads return promptly. */
static bool x_read_setup(void) {
    uint8_t hdr[8];
    if (!x_read_full(hdr, 8)) { fprintf(stderr, "[xwm] setup-reply read failed (%s)\n", strerror(errno)); return false; }
    if (hdr[0] != 1) {          /* 1 = Success (0 = Failed, 2 = Authenticate) */
        /* hdr[1] = reason length for a Failed reply; read + log it. */
        uint16_t addlen = get16(hdr + 6);
        char reason[128]; size_t rl = hdr[1] < sizeof(reason) - 1 ? hdr[1] : sizeof(reason) - 1;
        uint8_t tmp[512]; size_t bl = (size_t)addlen * 4; if (bl > sizeof tmp) bl = sizeof tmp;
        x_read_full(tmp, bl);
        memcpy(reason, tmp, rl); reason[rl] = '\0';
        fprintf(stderr, "[xwm] connection setup not accepted (code %u): %s\n", hdr[0], reason);
        return false;
    }
    uint16_t addlen = get16(hdr + 6);           /* additional data, 4-byte units */
    size_t bodylen = (size_t)addlen * 4;
    uint8_t *body = malloc(bodylen);
    if (!body || !x_read_full(body, bodylen)) { free(body); return false; }

    /* body layout (offsets within the additional-data block):
     *   0  release-number (4)          16 vendor-length (2)
     *   4  resource-id-base (4)        18 maximum-request-length (2)
     *   8  resource-id-mask (4)        20 number-of-SCREENS (1)
     *   12 motion-buffer-size (4)      21 number-of-FORMATS (1)
     *   22 image-byte-order (1) 23 bitmap-bit-order (1)
     *   24 scanline-unit (1) 25 scanline-pad (1)
     *   26 min-keycode (1) 27 max-keycode (1)
     *   28 pad (4)
     *   32 vendor (vendor-length, padded to 4)
     *   .. pixmap-FORMATs (8 * number-of-FORMATS)
     *   .. SCREENs (first field of the first SCREEN is the root WINDOW)
     * NOTE: the fixed header is 32 bytes, not 24 — the root lives after the
     * vendor + formats that follow it. */
    X.id_base = get32(body + 4);
    X.id_mask = get32(body + 8);
    uint16_t vendor_len = get16(body + 16);
    uint8_t  nformats = body[21];
    size_t off = 32;
    off += (vendor_len + 3u) & ~3u;             /* vendor, padded */
    off += (size_t)nformats * 8;                /* pixmap formats */
    /* first SCREEN: root window is the first field */
    if (off + 4 <= bodylen) X.root = get32(body + off);
    free(body);
    if (!X.root) { fprintf(stderr, "[xwm] no root window in setup\n"); return false; }
    return true;
}

/* Read InternAtom reply (blocking) during init and return the atom id. */
static uint32_t x_read_intern_reply(void) {
    uint8_t rep[32];
    if (!x_read_full(rep, 32)) return 0;
    if (rep[0] == XE_Error) return 0;
    if (rep[0] != XE_Reply) return 0;           /* no events before we select any */
    return get32(rep + 8);                      /* atom id */
}

static void x_intern_all(void) {
    /* Send all requests, then read replies in order (no events yet — we have not
     * selected SubstructureRedirect, so the server sends us nothing but replies). */
    x_intern_atom("WL_SURFACE_ID");
    x_intern_atom("WL_SURFACE_SERIAL");
    x_intern_atom("WM_PROTOCOLS");
    x_intern_atom("WM_DELETE_WINDOW");
    x_intern_atom("_NET_WM_NAME");
    x_intern_atom("WM_NAME");
    x_intern_atom("UTF8_STRING");
    x_intern_atom("_NET_ACTIVE_WINDOW");
    x_intern_atom("_NET_WM_STATE");
    x_intern_atom("_NET_WM_STATE_FULLSCREEN");
    x_intern_atom("WM_STATE");
    x_intern_atom("_NET_SUPPORTED");
    X.a_wl_surface_id    = x_read_intern_reply();
    X.a_wl_surface_serial= x_read_intern_reply();
    X.a_wm_protocols     = x_read_intern_reply();
    X.a_wm_delete        = x_read_intern_reply();
    X.a_net_wm_name      = x_read_intern_reply();
    X.a_wm_name          = x_read_intern_reply();
    X.a_utf8             = x_read_intern_reply();
    X.a_net_active       = x_read_intern_reply();
    X.a_net_wm_state     = x_read_intern_reply();
    X.a_net_wm_state_fs  = x_read_intern_reply();
    X.a_wm_state         = x_read_intern_reply();
    X.a_net_supported    = x_read_intern_reply();
}

/* ── Event handling ───────────────────────────────────────────────────────── */
static void ev_create(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    xwin_t *w = xwin_add(window);
    if (!w) return;
    w->x = (int16_t)get16(e + 12);
    w->y = (int16_t)get16(e + 14);
    w->w = get16(e + 16);
    w->h = get16(e + 18);
    w->override_redirect = e[22] != 0;
}

static void ev_map_request(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    xwin_t *w = xwin_add(window);
    x_change_event_mask(window, EV_PROPERTY_CHANGE);   /* watch title/serial */
    x_set_wm_state(window, 1 /*Normal*/);              /* ICCCM: window is managed */
    /* Maximize the window to fill the rootful X screen so the app has no black
     * X-root border around it. (Rootful presents the whole screen as one FiFi
     * window; filling it with the app makes them coincide.) */
    if (w) { w->x = 0; w->y = 0; w->w = s_screen_w; w->h = s_screen_h; }
    x_configure(window, 0, 0, s_screen_w, s_screen_h, false);
    x_map_window(window);       /* honor the map (we are the redirect target) */
    /* Complete the ICCCM handshake so the client finalizes layout + paints:
     * tell it its final geometry and give it input focus. */
    x_send_configure_notify(window, 0, 0, s_screen_w, s_screen_h);
    x_set_input_focus(window);
    /* Title the FiFi window with the app name. fifi-run writes it to a file
     * (reliable); fall back to the X window's _NET_WM_NAME/WM_NAME. */
    {
        int f = open("/tmp/fifi-x11-title", O_RDONLY);
        if (f >= 0) {
            char nm[128]; ssize_t n = read(f, nm, sizeof nm - 1); close(f);
            if (n > 0) { nm[n] = '\0'; wayland_x11_root_title(nm); }
        }
    }
    if (X.a_net_wm_name) x_get_property(window, X.a_net_wm_name);
    if (X.a_wm_name)     x_get_property(window, X.a_wm_name);
}

static void ev_map_notify(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    bool override = e[12] != 0;
    xwin_t *w = xwin_find(window);
    if (!w) { w = xwin_add(window); }
    if (!w) return;
    w->override_redirect = w->override_redirect || override;
    w->mapped = true;
    /* Watch for property changes (title, and WL_SURFACE_SERIAL if it is stamped
     * after map) then read the serial now. XWayland stamps the X window with
     * WL_SURFACE_SERIAL (2x CARD32) to pair it to the Wayland surface it created
     * via xwayland_shell_v1. */
    x_change_event_mask(window, EV_PROPERTY_CHANGE);
    if (X.a_wl_surface_serial) x_get_property(window, X.a_wl_surface_serial);
    xwin_try_adopt(w);
}

static void ev_unmap(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    xwin_t *w = xwin_find(window);
    if (w && w->adopted) wayland_x11_unmap(window);
    if (w) { w->mapped = false; w->adopted = false; }
}

static void ev_destroy(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    xwin_t *w = xwin_find(window);
    if (w && w->adopted) wayland_x11_unmap(window);
    xwin_remove(window);
}

static void ev_configure_request(const uint8_t *e) {
    uint32_t window = get32(e + 8);
    int32_t x = (int16_t)get16(e + 16);
    int32_t y = (int16_t)get16(e + 18);
    int32_t w = get16(e + 20);
    int32_t h = get16(e + 22);
    uint16_t mask = get16(e + 26);
    xwin_t *xw = xwin_find(window);
    if (xw) {
        if (mask & CFG_X) xw->x = x;
        if (mask & CFG_Y) xw->y = y;
        if (mask & CFG_W) xw->w = w;
        if (mask & CFG_H) xw->h = h;
    }
    /* Honor the requested geometry so the client is not left unconfigured. */
    x_configure(window, xw ? xw->x : x, xw ? xw->y : y,
                (mask & CFG_W) ? w : 0, (mask & CFG_H) ? h : 0, false);
    if (xw && xw->adopted)
        wayland_x11_geometry(window, xw->x, xw->y, xw->w, xw->h);
}

static void ev_client_message(const uint8_t *e) {
    (void)e;   /* legacy WL_SURFACE_ID path unused: XWayland 24 uses the serial. */
}

static void ev_property(const uint8_t *e) {
    uint32_t window = get32(e + 4);
    uint32_t atom   = get32(e + 8);
    if (atom == X.a_wl_surface_serial) {
        xwin_t *w = xwin_find(window);
        if (w && !w->adopted) x_get_property(window, atom);   /* serial arrived */
    } else if (atom == X.a_net_wm_name || atom == X.a_wm_name) {
        xwin_t *w = xwin_find(window);
        if (w) x_get_property(window, atom);   /* re-read; rootful relabels too */
    }
}

/* GetProperty reply: either WL_SURFACE_SERIAL (2x CARD32 -> 64-bit serial) or a
 * title string. Matched to the window + property atom via the pending table. */
static void reply_get_property(const uint8_t *rep, const uint8_t *value, uint32_t vlen, uint32_t seq) {
    uint32_t window = 0, atom = 0;
    for (int i = 0; i < X.n_prop_pend; i++)
        if (X.prop_pend[i].seq == seq) {
            window = X.prop_pend[i].window;
            atom   = X.prop_pend[i].atom;
            X.prop_pend[i] = X.prop_pend[--X.n_prop_pend];
            break;
        }
    (void)rep;
    if (!window) return;
    xwin_t *w = xwin_find(window);
    if (!w) return;
    if (atom == X.a_wl_surface_serial) {
        if (vlen >= 8) {                     /* lo (CARD32) + hi (CARD32) */
            uint32_t lo = get32(value), hi = get32(value + 4);
            w->serial = ((uint64_t)hi << 32) | lo;
            w->serial_known = true;
            fprintf(stderr, "[xwm] win 0x%x serial=%llu\n", window,
                    (unsigned long long)w->serial);
            xwin_try_adopt(w);
        }
        return;
    }
    /* title */
    if (!vlen) return;
    uint32_t n = vlen < sizeof(w->title) - 1 ? vlen : (uint32_t)(sizeof(w->title) - 1);
    memcpy(w->title, value, n);
    w->title[n] = '\0';
    if (w->adopted) wayland_x11_title(window, w->title);   /* rootless: per-window */
    else            wayland_x11_root_title(w->title);      /* rootful: relabel root */
}

/* Process one complete message from rbuf starting at offset 0; return the number
 * of bytes consumed, or 0 if a full message is not yet buffered. */
static int x_process_one(void) {
    if (X.rused < 32) return 0;
    uint8_t code = X.rbuf[0] & 0x7f;
    if (code == XE_Reply) {
        uint32_t extra = get32(X.rbuf + 4) * 4;
        if ((uint32_t)X.rused < 32 + extra) return 0;   /* wait for the whole reply */
        uint32_t seq = get16(X.rbuf + 2);
        /* Only GetProperty replies are awaited post-init (title fetches). */
        uint32_t vlen_units = get32(X.rbuf + 16);       /* value length in format units */
        uint8_t fmt = X.rbuf[1];
        uint32_t vbytes = vlen_units * (fmt == 32 ? 4 : fmt == 16 ? 2 : 1);
        if (vbytes > extra) vbytes = extra;
        reply_get_property(X.rbuf, X.rbuf + 32, vbytes, seq);
        return (int)(32 + extra);
    }
    if (code == XE_Error) {
        fprintf(stderr, "[xwm] X error: code=%u seq=%u major=%u\n",
                X.rbuf[1], get16(X.rbuf + 2), X.rbuf[10]);
        return 32;
    }
    switch (code) {
        case XE_CreateNotify:      ev_create(X.rbuf); break;
        case XE_MapRequest:        ev_map_request(X.rbuf); break;
        case XE_MapNotify:         ev_map_notify(X.rbuf); break;
        case XE_UnmapNotify:       ev_unmap(X.rbuf); break;
        case XE_DestroyNotify:     ev_destroy(X.rbuf); break;
        case XE_ConfigureRequest:  ev_configure_request(X.rbuf); break;
        case XE_ClientMessage:     ev_client_message(X.rbuf); break;
        case XE_PropertyNotify:    ev_property(X.rbuf); break;
        default: break;            /* ReparentNotify/ConfigureNotify: no-op */
    }
    return 32;
}

/* ── Public API ───────────────────────────────────────────────────────────── */
bool xwm_active(void) { return X.up; }
int  xwm_fd(void)     { return X.up ? X.fd : -1; }

bool xwm_init(void) {
    if (X.up) return true;
    /* Respawn backoff so a crash-looping XWayland can't fork-bomb. */
    static struct timespec s_last; static bool s_have_last = false;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);

    if (s_xwl_pid < 0) {
        if (s_have_last) {
            long ms = (now.tv_sec - s_last.tv_sec) * 1000 +
                      (now.tv_nsec - s_last.tv_nsec) / 1000000;
            if (ms < 3000) return false;
        }
        s_last = now; s_have_last = true;
        memset(&X, 0, sizeof X);
        X.fd = -1;
        int fd = xwl_spawn();
        if (fd < 0) { s_xwl_pid = -1; return false; }
        X.fd = fd;
        s_setup_sent = x_send_setup();   /* XWayland replies once it has started */
        return false;
    }

    /* XWayland spawned: wait (non-blocking) until it replies to our setup. */
    if (!s_setup_sent) { s_setup_sent = x_send_setup(); return false; }
    struct pollfd pfd = { .fd = X.fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
        /* Reap XWayland if it died before talking to us; respawn later. */
        if (s_xwl_pid > 0 && waitpid(s_xwl_pid, NULL, WNOHANG) == s_xwl_pid) {
            s_xwl_pid = -1; if (X.fd >= 0) close(X.fd); X.fd = -1; s_setup_sent = false;
        }
        return false;
    }
    if (!x_read_setup()) { fprintf(stderr, "[xwm] setup read failed\n"); xwm_shutdown(); return false; }
    x_intern_all();
    /* Become the window manager: redirect substructure on the root. */
    x_change_event_mask(X.root, EV_SUBSTRUCT_REDIR | EV_SUBSTRUCT_NOTIF);
    /* Advertise a minimal EWMH so GTK detects a compliant WM (it queries
     * _NET_SUPPORTED before it fully maps/paints). */
    if (X.a_net_supported) {
        uint32_t supported[] = { X.a_net_wm_state, X.a_net_wm_state_fs,
                                 X.a_net_active };
        x_change_prop32(X.root, X.a_net_supported, 4 /*ATOM*/, supported, 3);
    }
    /* Non-blocking from here on; the compositor poll loop drives us. */
    fcntl(X.fd, F_SETFL, fcntl(X.fd, F_GETFL, 0) | O_NONBLOCK);
    X.up = true;
    /* Signal readiness so fifi-run launches the X app only after we are the WM
     * (else the app's window maps + sends WL_SURFACE_ID before we can see it). */
    { int rf = open("/tmp/xwm.ready", O_CREAT | O_WRONLY | O_TRUNC, 0644);
      if (rf >= 0) close(rf); }
    fprintf(stderr, "[xwm] managing X server :0 (root=0x%x, atoms wl=%u del=%u)\n",
            X.root, X.a_wl_surface_id, X.a_wm_delete);
    return true;
}

void xwm_poll(void) {
    if (!X.up) return;
    for (;;) {
        if (X.rused >= (int)sizeof(X.rbuf)) { X.rused = 0; break; } /* overflow guard */
        ssize_t n = read(X.fd, X.rbuf + X.rused, sizeof(X.rbuf) - (size_t)X.rused);
        if (n > 0) {
            X.rused += (int)n;
            for (;;) {
                int used = x_process_one();
                if (used <= 0) break;
                memmove(X.rbuf, X.rbuf + used, (size_t)(X.rused - used));
                X.rused -= used;
            }
            continue;
        }
        if (n == 0) { xwm_shutdown(); return; }          /* XWayland exited */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        xwm_shutdown(); return;                          /* hard error */
    }
    /* Retry adoption for windows whose serial is known + mapped but whose
     * Wayland surface's set_serial had not been processed yet when we first
     * tried (the X and Wayland streams race). Cheap: usually 0-1 pending. */
    for (int i = 0; i < XWM_MAX_WINS; i++) {
        xwin_t *w = &X.wins[i];
        if (w->window && !w->adopted && w->mapped && w->serial_known)
            xwin_try_adopt(w);
    }
}

void xwm_shutdown(void) {
    unlink("/tmp/xwm.ready");
    /* Drop any adopted X windows still mapped. */
    for (int i = 0; i < XWM_MAX_WINS; i++)
        if (X.wins[i].window && X.wins[i].adopted)
            wayland_x11_unmap(X.wins[i].window);
    if (X.fd >= 0) close(X.fd);
    /* The WM connection is gone, so XWayland is unusable to us: terminate it so
     * xwm_init respawns a clean instance (guarded by the 3s backoff). */
    if (s_xwl_pid > 0) {
        kill(s_xwl_pid, SIGTERM);
        waitpid(s_xwl_pid, NULL, WNOHANG);
    }
    s_xwl_pid = -1;
    s_setup_sent = false;
    memset(&X, 0, sizeof X);
    X.fd = -1;
}

void xwm_configure(uint32_t xwindow, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!X.up) return;
    xwin_t *win = xwin_find(xwindow);
    if (win) { win->x = x; win->y = y; if (w > 0) win->w = w; if (h > 0) win->h = h; }
    x_configure(xwindow, x, y, w, h, false);
}

void xwm_set_focus(uint32_t xwindow) {
    if (!X.up) return;
    x_set_input_focus(xwindow);
}

void xwm_close(uint32_t xwindow) {
    if (!X.up) return;
    x_send_delete(xwindow);
}

void xwm_activate(uint32_t xwindow) {
    if (!X.up) return;
    x_configure(xwindow, 0, 0, 0, 0, true);     /* raise only */
    x_set_input_focus(xwindow);
}
