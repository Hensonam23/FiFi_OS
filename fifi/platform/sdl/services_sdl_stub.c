/* SDL dev runner has no IPC server, Wayland server, gamepad, or gaming
 * mode; the GUI sources probe these compositor services, so give them
 * inert answers. */
#include <stdint.h>
#include <stdbool.h>

uint32_t ipc_topmost_z(void)                    { return 0; }
uint32_t ipc_topmost_z_at(int32_t mx, int32_t my) { (void)mx; (void)my; return 0; }
uint32_t ipc_topmost_z_in_rect(uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
                                                { (void)rx; (void)ry; (void)rw; (void)rh; return 0; }
bool     ipc_any_maximized(void)                { return false; }

bool     wayland_any_mapped(void)               { return false; }
bool     wayland_any_maximized(void)            { return false; }
bool     wayland_browser_present(void)          { return false; }
bool     wayland_close_active(void)             { return false; }
bool     wayland_covers(int32_t x, int32_t y)   { (void)x; (void)y; return false; }
bool     wayland_has_focus(void)                { return false; }
void     wayland_toplevel_activate(int idx)     { (void)idx; }
int      wayland_toplevel_count(void)           { return 0; }

bool     input_gamepad_connected(void)          { return false; }
bool     mouse_consume_rclick(int32_t *x, int32_t *y) { (void)x; (void)y; return false; }

bool     gaming_mode_active(void)               { return false; }
void     gaming_mode_set(bool on)               { (void)on; }
uint32_t compositor_fps(void)                   { return 0; }
