#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_RESIZE_H,
    CURSOR_RESIZE_V,
    CURSOR_TEXT,
    CURSOR_HAND,
    CURSOR_MOVE,
    CURSOR_COUNT
} cursor_type_t;

void  mouse_init(void);
void  mouse_irq_handler(void);
void  mouse_on_byte(uint8_t b);
void  mouse_get_state(int32_t *x, int32_t *y, bool *lbtn, bool *rbtn);
void  mouse_push_rel(int32_t dx, int32_t dy, bool lbtn, bool rbtn);
void  mouse_warp(int32_t x, int32_t y);
void  mouse_click(int32_t x, int32_t y);
bool  mouse_consume_click(int32_t *x, int32_t *y);
void  mouse_set_intellimouse(bool enabled);
int8_t mouse_consume_scroll(void);
void  mouse_cursor_update(void);
void  mouse_set_cursor(cursor_type_t type);
cursor_type_t mouse_get_cursor(void);
