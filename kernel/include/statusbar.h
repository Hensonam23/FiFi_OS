#pragma once
#include <stdint.h>

/* Call once right after console_init() to reserve the bar area. */
void statusbar_init(uint64_t fb_width);

/* Call from pit_on_tick() — redraws bar content once per second. */
void statusbar_on_tick(void);
