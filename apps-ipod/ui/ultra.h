/* PodBox Fruit Company Ultra motion curves. GPL version 2+. */
#ifndef PODBOX_UI_ULTRA_H
#define PODBOX_UI_ULTRA_H

#include <stdbool.h>

/* Progress and results are Q10: 0 to UI_ULTRA_ONE. */
#define UI_ULTRA_ONE 1024

/* A damped spring's step response at progress u: rises past ONE by about
 * 5% near u = 0.45 and settles on ONE by u = ONE. */
int ui_ultra_spring(unsigned u);
/* Ease-out with a short overshoot of about 4% that settles at the end. */
int ui_ultra_back(unsigned u);
/* Cubic ease-out, for motion that must not overshoot. */
int ui_ultra_out(unsigned u);
/* Linear interpolation from `from` to `to` by a Q10 weight, which may
 * exceed ONE. */
int ui_ultra_mix(int from, int to, int weight);

#endif
