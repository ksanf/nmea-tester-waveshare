#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { ACTIVITY_PULSE_ON_MS = 100, ACTIVITY_PULSE_OFF_MS = 80 };

typedef struct {
    uint32_t changed_ms;
    bool lit;
    bool pending;
} activity_pulse_t;

static inline void activity_pulse_reset(activity_pulse_t *pulse, uint32_t now_ms)
{
    *pulse = (activity_pulse_t){ .changed_ms = now_ms - ACTIVITY_PULSE_OFF_MS };
}

/* A minimum dark gap keeps continuous traffic visibly blinking. Time differences
 * are unsigned so the LVGL millisecond counter can wrap without sticking a LED. */
static inline bool activity_pulse_update(activity_pulse_t *pulse, bool activity, uint32_t now_ms)
{
    pulse->pending |= activity;
    uint32_t elapsed = now_ms - pulse->changed_ms;
    if (pulse->lit) {
        if (elapsed >= ACTIVITY_PULSE_ON_MS) {
            pulse->lit = false;
            pulse->changed_ms = now_ms;
        }
    } else if (pulse->pending && elapsed >= ACTIVITY_PULSE_OFF_MS) {
        pulse->lit = true;
        pulse->pending = false;
        pulse->changed_ms = now_ms;
    }
    return pulse->lit;
}
