#ifndef FIFI_LINUX_TOUCHPAD_MOTION_H
#define FIFI_LINUX_TOUCHPAD_MOTION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t raw_x;
    int32_t raw_y;
    int64_t residual_x_q16;
    int64_t residual_y_q16;
    bool anchored;
} touchpad_motion_state_t;

static inline void touchpad_motion_reset(touchpad_motion_state_t *state) {
    *state = (touchpad_motion_state_t){0};
}

static inline int64_t touchpad_motion_abs64(int64_t value) {
    return value < 0 ? -value : value;
}

/* Translate one absolute touchpad sample into immediate relative screen motion.
 * Slow movement remains one-to-one and accumulates below-pixel precision. Faster
 * movement gets modest acceleration, while discontinuities re-anchor instead of
 * throwing the pointer across the screen. X and Y use their own device ranges. */
static inline bool touchpad_motion_update(touchpad_motion_state_t *state,
                                          int32_t raw_x, int32_t raw_y,
                                          int32_t range_x, int32_t range_y,
                                          int32_t screen_w, int32_t screen_h,
                                          int32_t *out_dx, int32_t *out_dy) {
    if (out_dx) *out_dx = 0;
    if (out_dy) *out_dy = 0;
    if (!state || range_x <= 0 || range_y <= 0 ||
        screen_w <= 0 || screen_h <= 0) return false;

    if (!state->anchored) {
        state->raw_x = raw_x;
        state->raw_y = raw_y;
        state->anchored = true;
        return false;
    }

    int64_t raw_dx = (int64_t)raw_x - state->raw_x;
    int64_t raw_dy = (int64_t)raw_y - state->raw_y;
    state->raw_x = raw_x;
    state->raw_y = raw_y;

    if (touchpad_motion_abs64(raw_dx) > range_x / 8 ||
        touchpad_motion_abs64(raw_dy) > range_y / 8) {
        state->residual_x_q16 = 0;
        state->residual_y_q16 = 0;
        return false;
    }

    int64_t speed_x = touchpad_motion_abs64(raw_dx) * 1000 / range_x;
    int64_t speed_y = touchpad_motion_abs64(raw_dy) * 1000 / range_y;
    int64_t speed = speed_x > speed_y ? speed_x : speed_y;
    int64_t gain_q8 = speed <= 2 ? 256 : (speed < 8 ? 320 : 384);

    state->residual_x_q16 += raw_dx * screen_w * 256 * gain_q8 / range_x;
    state->residual_y_q16 += raw_dy * screen_h * 256 * gain_q8 / range_y;
    int32_t dx = (int32_t)(state->residual_x_q16 / 65536);
    int32_t dy = (int32_t)(state->residual_y_q16 / 65536);
    state->residual_x_q16 -= (int64_t)dx * 65536;
    state->residual_y_q16 -= (int64_t)dy * 65536;
    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
    return dx != 0 || dy != 0;
}

#endif
