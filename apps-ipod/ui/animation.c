/* PodBox UI animation clock. GPL version 2+. */
#include "animation.h"

#define CLOCK_HZ 1000000u
#define ONE 1024

unsigned ui_clock_frame(uint32_t elapsed_clock)
{
    return (uint64_t)elapsed_clock * UI_ANIM_FPS / CLOCK_HZ;
}

uint32_t ui_frame_clock(unsigned frame)
{
    return ((uint64_t)frame * CLOCK_HZ + UI_ANIM_FPS - 1) / UI_ANIM_FPS;
}

int ui_clock_poll_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second)
{
    unsigned frame = ui_clock_frame(elapsed);
    if (frame >= duration)
        return 0;

    uint32_t clock_left = ui_frame_clock(frame + 1) - elapsed;
    return (uint64_t)clock_left * ticks_per_second / CLOCK_HZ;
}

int ui_clock_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second)
{
    int ticks = ui_clock_poll_timeout(elapsed, duration, ticks_per_second);
    return ticks == 0 && ui_clock_frame(elapsed) < duration ? 1 : ticks;
}

int ui_tween_sample(const struct ui_tween *tween, unsigned frame)
{
    if (frame >= tween->frames)
        return tween->to;
    unsigned t = frame * ONE / tween->frames;
    unsigned u = tween->ease_in ? t : ONE - t;
    unsigned cube = u * u * u / (ONE * ONE);
    int weight = tween->ease_in ? cube : ONE - cube;
    return tween->from + (tween->to - tween->from) * weight / ONE;
}
