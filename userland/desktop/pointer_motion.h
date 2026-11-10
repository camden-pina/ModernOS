#ifndef COAST_POINTER_MOTION_H
#define COAST_POINTER_MOTION_H
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint64_t timestamp;
    int64_t velocity, fraction_x, fraction_y;
} PointerMotion;
static inline int32_t pointer_axis(int32_t raw, int64_t gain, int64_t *fraction) {
    int64_t value = (int64_t)raw * gain + *fraction;
    int64_t result = value / 65536;
    if (result > INT32_MAX) {
        *fraction = 0;
        return INT32_MAX;
    }
    if (result < INT32_MIN) {
        *fraction = 0;
        return INT32_MIN;
    }
    *fraction = value - result * 65536;
    return (int32_t)result;
}
/* Integer precision preserves subpixel movement. Acceleration responds to
 * counts/second with a 30ms filter, rather than the number of input packets.
 * It is an original bounded curve, not a copy of a proprietary OS curve. */
static inline void pointer_motion(PointerMotion *state, int speed, bool acceleration, int32_t dx,
                                  int32_t dy, uint64_t timestamp, int32_t *out_x, int32_t *out_y) {
    if (speed < 25)
        speed = 25;
    if (speed > 300)
        speed = 300;
    if (!acceleration && speed == 100) {
        state->timestamp = timestamp;
        state->velocity = 0;
        state->fraction_x = state->fraction_y = 0;
        *out_x = dx;
        *out_y = dy;
        return;
    }
    uint64_t elapsed =
        state->timestamp && timestamp >= state->timestamp ? timestamp - state->timestamp : 0;
    if (!state->timestamp || timestamp < state->timestamp || elapsed > 250000) {
        state->velocity = 0;
        elapsed = 8000;
        /* A fraction is real movement, even across a pause. Settings changes
           and screen boundaries explicitly clear it instead. */
    }
    state->timestamp = timestamp;
    if (elapsed < 1000)
        elapsed = 1000;
    if (elapsed > 50000)
        elapsed = 50000;
    int64_t x = dx < 0 ? -(int64_t)dx : dx, y = dy < 0 ? -(int64_t)dy : dy;
    int64_t instantaneous = (x > y ? x : y) * 1000000 / (int64_t)elapsed;
    if (instantaneous > 10000)
        instantaneous = 10000;
    instantaneous *= 65536;
    state->velocity +=
        (instantaneous - state->velocity) * (int64_t)elapsed / (30000 + (int64_t)elapsed);
    int64_t gain = 65536;
    if (acceleration) {
        int64_t fast = state->velocity / 65536 - 300;
        if (fast < 0)
            fast = 0;
        if (fast > 1700)
            fast = 1700;
        gain += fast * 65536 / 1700;
    }
    gain = gain * speed / 100;
    *out_x = pointer_axis(dx, gain, &state->fraction_x);
    *out_y = pointer_axis(dy, gain, &state->fraction_y);
}
/* Drop only movement beyond an edge. Inward subpixel movement must survive,
   otherwise a slow pointer can become stuck at the edge. */
static inline void pointer_clip_fraction(PointerMotion *state, int x, int y, int maximum_x,
                                         int maximum_y) {
    if ((x <= 0 && state->fraction_x < 0) || (x >= maximum_x && state->fraction_x > 0))
        state->fraction_x = 0;
    if ((y <= 0 && state->fraction_y < 0) || (y >= maximum_y && state->fraction_y > 0))
        state->fraction_y = 0;
}
#endif
