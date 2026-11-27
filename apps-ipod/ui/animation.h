/* PodBox UI animation clock. GPL version 2+. */
#ifndef PODBOX_UI_ANIMATION_H
#define PODBOX_UI_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>

/* Durations are authored in frames of 60 fps. An animation advances one
 * frame per panel refresh, and two per presented frame at half rate. */
#define UI_ANIM_FPS 60u
#define ANIM_F(frames) (frames)

struct ui_tween {
    uint32_t start_clock;
    int from, to;
    unsigned frames;
    bool ease_in;
    /* A UI_EASE_* curve from native_theme.h; zero is cubic. */
    uint8_t easing;
};

/* The refresh period one frame lasts, in 1/256 microseconds: the panel's
 * measured TE period, or nominal 66.96 Hz while TE is not measured. */
uint32_t ui_frame_period_q8(void);
/* The same, rounded to microseconds. */
uint32_t ui_frame_period(void);

/* Clock readings, including wraparound, are in USEC_TIMER units. */
unsigned ui_clock_frame(uint32_t elapsed_clock);
uint32_t ui_frame_clock(unsigned frame);
/* A start clock on the latest TE rise, so frames begin on refreshes. */
uint32_t ui_clock_start(void);
/* Kernel wait to the next frame this animation presents. Returns zero when
 * the animation has ended. The wait wakes precisely when the calling
 * thread's next input wait or ui_clock_sleep() takes it. */
int ui_clock_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second);
/* The same, but zero also means a frame is due now. */
int ui_clock_poll_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second);
/* Kernel ticks to deadline for an input wait, or zero when it has passed.
 * Arms the same precise wake. */
int ui_clock_wait_until(uint32_t deadline);
/* Sleeps for a timeout from the functions above, to its precise wake. */
void ui_clock_sleep(int ticks);
void ui_clock_sleep_until(uint32_t deadline);
int ui_tween_sample(const struct ui_tween *tween, unsigned frame);

#endif
