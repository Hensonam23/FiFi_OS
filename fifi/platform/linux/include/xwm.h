#ifndef FIFI_XWM_H
#define FIFI_XWM_H

#include <stdbool.h>
#include <stdint.h>

/* ── Minimal X11 window manager for rootless XWayland ────────────────────────
 * The compositor speaks Wayland natively (hand-rolled, see wayland.c). X11-only
 * apps (LibreOffice) run under a rootless XWayland which does NOT drive xdg-shell
 * for its windows: instead it expects a window manager to connect over the X11
 * protocol, take over window management, and correlate each X window to the
 * Wayland surface XWayland created for it (via the WL_SURFACE_ID client message).
 *
 * This module is that window manager. It hand-rolls the X11 wire protocol (no
 * XCB/Xlib — the compositor is a static, self-contained binary) and hands each
 * mapped X toplevel to wayland.c so it is presented as an ordinary FiFi window
 * (own titlebar, decorations, drag/resize, z-order) with no black X-root border.
 *
 * Lifecycle: the X server only exists while XWayland is running (started on
 * demand by fifi-run). xwm_init() connects when DISPLAY :0 is available; it is
 * safe to call repeatedly (no-op once connected). All entry points are called
 * from the compositor's single event thread under g_mx. */

/* Adopt callback (implemented in wayland.c): pair an X window to the Wayland
 * surface XWayland stamped with `serial` (xwayland_shell_v1) and present it.
 * Returns false if that surface has not appeared yet, so xwm retries. */
bool wayland_x11_adopt(uint64_t serial, uint32_t xwindow,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       bool decorated, const char *title);

void wayland_x11_root_title(const char *title);  /* relabel the rootful X window */

bool xwm_init(void);       /* connect to :0 + become WM; false if X not up yet   */
void xwm_poll(void);       /* drain + dispatch pending X events (under g_mx)      */
int  xwm_fd(void);         /* X socket fd for poll(), or -1 when not connected    */
void xwm_shutdown(void);   /* disconnect (e.g. XWayland died)                     */
bool xwm_active(void);     /* connected and managing                              */
int  xwm_x_window_count(void); /* mapped non-override X windows (0 = hide root)   */

/* ── Compositor -> X requests (called by wayland.c when the user acts) ───────
 * The FiFi-side window geometry is authoritative once the user drags/resizes;
 * these push that back to the X window so the app repaints at the right size. */
void xwm_configure(uint32_t xwindow, int32_t x, int32_t y, int32_t w, int32_t h);
void xwm_set_focus(uint32_t xwindow);   /* give X input focus to this window     */
void xwm_close(uint32_t xwindow);       /* polite close (WM_DELETE_WINDOW)        */
void xwm_activate(uint32_t xwindow);    /* raise + focus                          */

#endif /* FIFI_XWM_H */
