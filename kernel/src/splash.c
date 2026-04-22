/*
 * splash.c — Boot startup: clears terminal and resets colors.
 * The visual system info overlay is now drawn directly on the desktop
 * background by draw_desktop_info() in gui.c.
 */

#include "splash.h"
#include "console.h"

#define S_BG  0x001a1a2eu

void splash_show(void) {
    console_set_colors(0x00ffffffu, S_BG);
    console_clear();
    console_set_cursor(0u, 0u);
}

void splash_repaint(void) { /* no-op: desktop info drawn by gui.c */ }
