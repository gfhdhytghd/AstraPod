/* PodBox UI animation clock. GPL version 2+. */
#ifndef PODBOX_UI_ANIMATION_H
#define PODBOX_UI_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>

#define UI_ANIM_FPS 60u
#define ANIM_F(frames) (frames)

struct ui_tween {
    uint32_t start_clock;
    int from, to;
    unsigned frames;
    bool ease_in;
};

/* Clock readings, including wraparound, are in USEC_TIMER units. */
unsigned ui_clock_frame(uint32_t elapsed_clock);
uint32_t ui_frame_clock(unsigned frame);
/* Kernel wait to the next frame, rounded down so a wait cannot skip a frame
 * merely by rounding up. Returns zero when the animation has ended. */
int ui_clock_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second);
int ui_tween_sample(const struct ui_tween *tween, unsigned frame);

#endif
