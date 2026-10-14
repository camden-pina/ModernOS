#ifndef MODERNOS_SMOOTH_SCROLL_H
#define MODERNOS_SMOOTH_SCROLL_H

#include <stdbool.h>
#include <stdint.h>

/* Positions are pixels, including the fractional position between frames.
   The critically damped spring preserves velocity when another wheel tick
   arrives. Its clock is elapsed time, so movement does not depend on FPS. */
typedef struct {
    double position, velocity;
    int target, pixel;
    uint64_t updated;
    bool initialized, active;
} SmoothScroll;

static inline int smooth_bound(int64_t value, int maximum) {
    if (maximum < 0)
        maximum = 0;
    return value < 0 ? 0 : value > maximum ? maximum : (int)value;
}

static inline void smooth_jump(SmoothScroll *s, int position, uint64_t now) {
    s->position = s->target = s->pixel = position;
    s->velocity = 0;
    s->updated = now;
    s->initialized = true;
    s->active = false;
}

static inline void smooth_sync(SmoothScroll *s, int current, int maximum, uint64_t now) {
    int bounded = smooth_bound(current, maximum);
    /* Navigation, keyboard reveal and content replacement take precedence. */
    if (!s->initialized || current != s->pixel || bounded != current)
        smooth_jump(s, bounded, now);
    s->target = smooth_bound(s->target, maximum);
}

static inline void smooth_by(SmoothScroll *s, int current, int64_t delta, int maximum,
                             uint64_t now) {
    smooth_sync(s, current, maximum, now);
    s->target = smooth_bound((int64_t)s->target + delta, maximum);
    s->active = s->position != s->target;
    if (!s->active)
        s->velocity = 0;
}

static inline int smooth_tick(SmoothScroll *s, int current, int maximum, uint64_t now) {
    smooth_sync(s, current, maximum, now);
    uint64_t elapsed = now >= s->updated ? now - s->updated : 0;
    s->updated = now;
    if (!s->active)
        return s->pixel;
    /* A suspended/hidden view resumes at the destination, without catch-up
       work or a large stale animation queue. */
    if (elapsed > 250) {
        smooth_jump(s, s->target, now);
        return s->pixel;
    }
    while (elapsed) {
        unsigned step = elapsed > 8 ? 8 : (unsigned)elapsed;
        elapsed -= step;
        double dt = step / 1000.0, omega = 30.0;
        double x = omega * dt;
        double decay = 1.0 / (1.0 + x + .48 * x * x + .235 * x * x * x);
        double distance = s->position - s->target;
        double change = (s->velocity + omega * distance) * dt;
        double next = s->target + (distance + change) * decay;
        s->velocity = (s->velocity - omega * change) * decay;
        if ((distance < 0 && next > s->target) || (distance > 0 && next < s->target)) {
            next = s->target;
            s->velocity = 0;
        }
        s->position = next;
        if (next < 0 || next > maximum) {
            smooth_jump(s, smooth_bound((int64_t)next, maximum), now);
            break;
        }
    }

    double remaining = s->position - s->target;
    if (remaining > -.1 && remaining < .1 && s->velocity > -3 && s->velocity < 3)
        smooth_jump(s, s->target, now);
    s->pixel = smooth_bound((int64_t)(s->position + .5), maximum);
    return s->pixel;
}

#endif
