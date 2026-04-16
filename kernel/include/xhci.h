#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialize XHCI controller and enumerate USB keyboard.
 * Safe to call even if no XHCI controller is present. */
void xhci_init(void);

/* Poll event ring for keyboard input — call from pit_on_tick() */
void xhci_poll(void);

/* Diagnostic log — all [xhci] boot messages stored in a 4KB ring.
 * Viewable via `xhci-log` shell command. */
const char *xhci_log_get(void);
uint32_t    xhci_log_size(void);
