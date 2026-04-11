#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Initialize XHCI controller and enumerate USB keyboard.
 * Safe to call even if no XHCI controller is present. */
void xhci_init(void);

/* Poll event ring for keyboard input — call from pit_on_tick() */
void xhci_poll(void);
