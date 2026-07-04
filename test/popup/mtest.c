/* Minimal Wayland menu-repro client for the FiFi compositor.
 *
 * Mirrors how Firefox/GTK opens a menu: a mapped xdg_toplevel, then on a
 * left-click an xdg_popup WITH A GRAB, whose visible content lives in a
 * wl_subsurface (not the popup surface's own buffer). It logs every pointer
 * event and which surface it targeted, so we can see whether the compositor
 * routes hover/clicks INTO the menu subsurface (menu works) or drops them on
 * the toplevel behind it (the bug we're chasing).
 *
 * All output goes to stderr (the test initramfs points that at the serial
 * console so the host can read it).
 */
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

static struct wl_compositor   *compositor;
static struct wl_shm          *shm;
static struct wl_subcompositor *subcompositor;
static struct wl_seat         *seat;
static struct xdg_wm_base     *wm_base;
static struct wl_pointer      *pointer;

static struct wl_surface  *top_surf, *popup_surf, *menu_surf;
static struct xdg_surface *top_xdg,  *popup_xdg;
static struct xdg_toplevel *toplevel;
static struct xdg_popup   *popup;
static uint32_t last_serial = 0;   /* latest input serial (for grab) */
static int popup_open = 0;

#define TAG(s) ((s)==top_surf?"TOPLEVEL":(s)==popup_surf?"POPUP":(s)==menu_surf?"MENU-SUB":"?")

static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("buf", 0);
    if (ftruncate(fd, size) < 0) { perror("ftruncate"); }
    uint32_t *px = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w*h; i++) px[i] = argb;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ── pointer listener ─────────────────────────────────────────────────── */
static struct wl_surface *focused_surf = NULL;

/* Replicate GTK menu-grab behaviour: while the menu is open, a pointer event
 * that lands on a NON-menu surface (the toplevel/content) means the grab isn't
 * holding the pointer, so GTK dismisses the menu. This is exactly the
 * "closes as soon as I move off the button" symptom. */
static void maybe_dismiss(const char *why) {
    if (popup_open && focused_surf != menu_surf && focused_surf != popup_surf) {
        fprintf(stderr, "MTEST: DISMISS menu (%s: pointer on %s during grab)\n",
                why, TAG(focused_surf));
        xdg_popup_destroy(popup); popup = NULL;
        if (menu_surf) { wl_surface_destroy(menu_surf); menu_surf = NULL; }
        wl_surface_destroy(popup_surf); popup_surf = NULL;
        popup_open = 0;
    }
}

/* Firefox crashes on pointer coords outside the menu surface; flag any here. */
static void check_oob(struct wl_surface *surf, int x, int y) {
    if (surf == menu_surf && (x < 0 || x >= 200 || y < 0 || y >= 300))
        fprintf(stderr, "MTEST !!! OOB coords on MENU-SUB: (%d,%d) [would crash Firefox]\n", x, y);
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial,
                    struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y) {
    last_serial = serial;
    focused_surf = surf;
    fprintf(stderr, "MTEST ptr.enter  surf=%s at (%d,%d)\n",
            TAG(surf), wl_fixed_to_int(x), wl_fixed_to_int(y));
    check_oob(surf, wl_fixed_to_int(x), wl_fixed_to_int(y));
    maybe_dismiss("enter");
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial,
                    struct wl_surface *surf) {
    last_serial = serial;
    fprintf(stderr, "MTEST ptr.leave  surf=%s\n", TAG(surf));
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t,
                     wl_fixed_t x, wl_fixed_t y) {
    static int n = 0;
    if ((n++ & 15) == 0)
        fprintf(stderr, "MTEST ptr.motion (%d,%d) on %s\n",
                wl_fixed_to_int(x), wl_fixed_to_int(y), TAG(focused_surf));
    check_oob(focused_surf, wl_fixed_to_int(x), wl_fixed_to_int(y));
    maybe_dismiss("motion");
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial,
                     uint32_t t, uint32_t button, uint32_t state) {
    last_serial = serial;
    fprintf(stderr, "MTEST ptr.button btn=%u state=%u\n", button, state);
    /* On left press (0x110) with no popup open, open the menu. */
    if (button == 0x110 && state == 1 && !popup_open) {
        fprintf(stderr, "MTEST >>> opening popup menu (grab serial=%u)\n", serial);
        struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
        xdg_positioner_set_size(pos, 200, 300);
        /* anchor rect near where we clicked in the toplevel */
        xdg_positioner_set_anchor_rect(pos, 100, 100, 1, 1);
        xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

        popup_surf = wl_compositor_create_surface(compositor);
        popup_xdg  = xdg_wm_base_get_xdg_surface(wm_base, popup_surf);
        extern const struct xdg_surface_listener xdg_surf_listener;
        xdg_surface_add_listener(popup_xdg, &xdg_surf_listener, NULL);
        popup = xdg_surface_get_popup(popup_xdg, top_xdg, pos);
        extern const struct xdg_popup_listener popup_listener;
        xdg_popup_add_listener(popup, &popup_listener, NULL);
        xdg_popup_grab(popup, seat, serial);
        xdg_positioner_destroy(pos);
        wl_surface_commit(popup_surf);   /* map role; buffer attached on configure */
        popup_open = 1;
    }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_src(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_axis_dir(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener pointer_listener = {
    p_enter, p_leave, p_motion, p_button, p_axis, p_frame,
    p_axis_src, p_axis_stop, p_axis_disc, p_axis_v120, p_axis_dir,
};

/* ── popup listeners ──────────────────────────────────────────────────── */
static void popup_configure(void *d, struct xdg_popup *pp, int32_t x, int32_t y,
                            int32_t w, int32_t h) {
    fprintf(stderr, "MTEST popup.configure pos=(%d,%d) size=%dx%d\n", x, y, w, h);
}
static void popup_done(void *d, struct xdg_popup *pp) {
    fprintf(stderr, "MTEST popup.done (dismissed)\n");
    popup_open = 0;
}
static void popup_repos(void *d, struct xdg_popup *pp, uint32_t t) {}
const struct xdg_popup_listener popup_listener = { popup_configure, popup_done, popup_repos };

static void xdg_surf_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
    xdg_surface_ack_configure(xs, serial);
    if (xs == popup_xdg && popup_surf) {
        /* Put the visible menu in a SUBSURFACE of the popup, and DO NOT attach a
         * buffer to the popup surface itself — this mirrors Firefox, which leaves
         * the xdg_popup's own wl_surface un-mapped (content lives in the
         * subsurface). The compositor must still treat the grab as active. */
        menu_surf = wl_compositor_create_surface(compositor);
        struct wl_subsurface *sub =
            wl_subcompositor_get_subsurface(subcompositor, menu_surf, popup_surf);
        wl_subsurface_set_position(sub, 0, 0);
        wl_subsurface_set_desync(sub);
        struct wl_buffer *mbuf = make_buffer(200, 300, 0xff3060c0);  /* menu content */
        wl_surface_attach(menu_surf, mbuf, 0, 0);
        wl_surface_commit(menu_surf);
        wl_surface_commit(popup_surf);
        fprintf(stderr, "MTEST popup mapped with menu subsurface\n");
    }
}
const struct xdg_surface_listener xdg_surf_listener = { xdg_surf_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *st) {}
static void tl_close(void *d, struct xdg_toplevel *t) {}
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener toplevel_listener = {
    tl_configure, tl_close, tl_bounds, tl_caps,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_subcompositor"))
        subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 3);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, "wl_seat")) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "MTEST: cannot connect to Wayland\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);   /* seat caps */

    if (!compositor || !shm || !wm_base || !subcompositor) {
        fprintf(stderr, "MTEST: missing globals c=%p shm=%p wm=%p sub=%p\n",
                (void*)compositor,(void*)shm,(void*)wm_base,(void*)subcompositor);
        return 1;
    }

    top_surf = wl_compositor_create_surface(compositor);
    top_xdg  = xdg_wm_base_get_xdg_surface(wm_base, top_surf);
    xdg_surface_add_listener(top_xdg, &xdg_surf_listener, NULL);
    toplevel = xdg_surface_get_toplevel(top_xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "mtest");
    wl_surface_commit(top_surf);
    wl_display_roundtrip(dpy);
    /* attach toplevel buffer */
    struct wl_buffer *tb = make_buffer(600, 400, 0xff101820);
    wl_surface_attach(top_surf, tb, 0, 0);
    wl_surface_commit(top_surf);

    fprintf(stderr, "MTEST: ready — left-click the window to open the menu\n");
    while (wl_display_dispatch(dpy) != -1) { }
    return 0;
}
