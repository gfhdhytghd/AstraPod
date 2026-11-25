/* PodBox UI animation clock. GPL version 2+. */
#include "animation.h"

#define ONE 1024

unsigned ui_clock_frame(uint32_t elapsed_clock)
{
    return ((uint64_t)elapsed_clock << 8) / ui_frame_period_q8();
}

uint32_t ui_frame_clock(unsigned frame)
{
    return ((uint64_t)frame * ui_frame_period_q8() + 255) >> 8;
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
