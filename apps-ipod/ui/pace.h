/* PodBox UI frame pacing. GPL version 2+. */
#ifndef PODBOX_UI_PACE_H
#define PODBOX_UI_PACE_H

#include <stdbool.h>
#include <stdint.h>

/* One animated loop's presentation schedule. Zero-initialise and keep it
 * across animations: the rate and wake phase it learns carry over. */
struct ui_pace {
    const void *site;
    uint32_t start, due, offset, cost, dma, busy;
    unsigned frame, shown, missed, transfers;
    uint8_t divisor, late, on_time;
    bool running, async, recover_full_rate;
};

struct ui_pace_status {
    uint32_t period, offset, cost, dma, busy;
    unsigned shown, missed;
    uint8_t divisor;
    bool enabled, measured, phased, async;
};

/* TE pacing: on, frames follow the panel refresh and a loop that cannot
 * finish a frame within one refresh presents every second one. Off, frames
 * run at the authored 60 fps on the free-running clock. */
extern bool ui_te_pacing;

/* The deadline of the next frame to present for the animation that began
 * at start. Call after drawing each frame; the time since the last
 * deadline is that frame's cost. Pass the result to ui_clock_wait_until(). */
uint32_t ui_pace_next(struct ui_pace *pace, uint32_t start);
/* The animation's frame due at the deadline ui_pace_next() returned. */
static inline unsigned ui_pace_frame(const struct ui_pace *pace)
{
    return pace->frame;
}
/* The pace last scheduled, for diagnostics. */
void ui_pace_get_status(struct ui_pace_status *status);
/* Microseconds after the predicted TE rise, or UINT32_MAX when unknown. */
uint32_t ui_clock_phase(uint32_t clock);

#endif
