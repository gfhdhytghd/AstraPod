/* PodBox Fruit Company Ultra motion curves. GPL version 2+.
 *
 * Shared by the Ultra motion of every screen. Each curve is fixed point
 * and table driven, so sampling one costs a few instructions per frame. */
#include "ultra.h"

#define STEPS 64

/* 1 - e^(-zwt)(cos(w't) + zw/w' sin(w't)), z = 0.68, w = 9, sampled at
 * t = i/64 and pinned to ONE at the end. */
static const short spring[STEPS + 1] = {
    0, 9, 36, 75, 124, 181, 244, 309, 376, 443, 509, 572, 633, 690, 743,
    792, 836, 877, 913, 944, 972, 995, 1015, 1032, 1046, 1057, 1065, 1071,
    1076, 1078, 1079, 1079, 1078, 1077, 1074, 1071, 1068, 1064, 1061, 1057,
    1054, 1050, 1047, 1044, 1041, 1038, 1035, 1033, 1031, 1029, 1028, 1026,
    1025, 1024, 1023, 1022, 1022, 1022, 1021, 1021, 1021, 1021, 1021, 1021,
    1024,
};

int ui_ultra_spring(unsigned u)
{
    if (u >= UI_ULTRA_ONE)
        return UI_ULTRA_ONE;
    unsigned i = u / (UI_ULTRA_ONE / STEPS);
    int frac = u % (UI_ULTRA_ONE / STEPS);
    return spring[i] + (spring[i + 1] - spring[i]) * frac /
                       (UI_ULTRA_ONE / STEPS);
}

int ui_ultra_back(unsigned u)
{
    if (u >= UI_ULTRA_ONE)
        return UI_ULTRA_ONE;
    /* 1 + c3 (u-1)^3 + c1 (u-1)^2 with c1 = 0.9, c3 = c1 + 1 */
    int v = (int)u - UI_ULTRA_ONE;
    int v2 = v * v / UI_ULTRA_ONE, v3 = v2 * v / UI_ULTRA_ONE;
    return UI_ULTRA_ONE + 1946 * v3 / UI_ULTRA_ONE + 922 * v2 / UI_ULTRA_ONE;
}

int ui_ultra_out(unsigned u)
{
    if (u >= UI_ULTRA_ONE)
        return UI_ULTRA_ONE;
    int v = UI_ULTRA_ONE - (int)u;
    return UI_ULTRA_ONE - v * v / UI_ULTRA_ONE * v / UI_ULTRA_ONE;
}

int ui_ultra_mix(int from, int to, int weight)
{
    return from + (to - from) * weight / UI_ULTRA_ONE;
}
