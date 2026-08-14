#ifndef FIFI_LINUX_TOUCHPAD_AXIS_H
#define FIFI_LINUX_TOUCHPAD_AXIS_H

#include <stdint.h>

typedef struct {
    int32_t x[2];
    int32_t y[2];
    int current_slot;
} touchpad_axis_state_t;

static inline void touchpad_axis_reset(touchpad_axis_state_t *state) {
    state->x[0] = state->x[1] = -1;
    state->y[0] = state->y[1] = -1;
    state->current_slot = 0;
}

static inline void touchpad_axis_select_slot(touchpad_axis_state_t *state, int slot) {
    state->current_slot = slot;
}

static inline void touchpad_axis_set_x(touchpad_axis_state_t *state, int32_t value) {
    if (state->current_slot >= 0 && state->current_slot < 2)
        state->x[state->current_slot] = value;
}

static inline void touchpad_axis_set_y(touchpad_axis_state_t *state, int32_t value) {
    if (state->current_slot >= 0 && state->current_slot < 2)
        state->y[state->current_slot] = value;
}

#endif
