/* PodBox UI animation clock. GPL version 2+. */
#include "animation.h"
#include "native_theme.h"

#define ONE 1024

unsigned ui_clock_frame(uint32_t elapsed_clock)
{
    return ((uint64_t)elapsed_clock << 8) / ui_frame_period_q8();
}

uint32_t ui_frame_clock(unsigned frame)
{
    return ((uint64_t)frame * ui_frame_period_q8() + 255) >> 8;
}

/* sin(x * pi / 2) for x in [0, ONE], by its odd series to x^7. */
static unsigned quarter_sine(unsigned x)
{
    uint64_t q = (uint64_t)x << 6, q2 = q * q >> 16;
    uint64_t poly = 102944 - (q2 * (42334 - (q2 * (5223 - (q2 * 307 >> 16))
                                             >> 16)) >> 16);
    unsigned s = (q * poly >> 16) >> 6;
    return s > ONE ? ONE : s;
}

/* The ease-in shape of each curve: 0 at u = 0, ONE at u = ONE. */
static unsigned ease_in(unsigned easing, unsigned u)
{
    unsigned square = u * u / ONE;
    switch (easing)
    {
    case UI_EASE_QUAD:
        return square;
    case UI_EASE_QUINT:
        return square * square / ONE * u / ONE;
    case UI_EASE_SINE:
        return ONE - quarter_sine(ONE - u);
    case UI_EASE_LINEAR:
        return u;
    default:
        return u * u * u / (ONE * ONE);
    }
}

int ui_tween_sample(const struct ui_tween *tween, unsigned frame)
{
    if (frame >= tween->frames)
        return tween->to;
    unsigned t = frame * ONE / tween->frames;
    int weight = tween->ease_in ? (int)ease_in(tween->easing, t)
                                : ONE - (int)ease_in(tween->easing, ONE - t);
    return tween->from + (tween->to - tween->from) * weight / ONE;
}
