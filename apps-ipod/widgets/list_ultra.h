/* PodBox Fruit Company Ultra list motion. GPL version 2+.
 *
 * The list renderer asks these when ui_theme_ultra() holds: the spring the
 * selection bar and rows glide on, the glint across the bar once it
 * settles, the stock marquee run a pixel a refresh, and the scroll
 * indicator's fade. Each is a pure function of time; the renderer keeps
 * the clocks. */
#ifndef PODBOX_LIST_ULTRA_H
#define PODBOX_LIST_ULTRA_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/retained.h"

/* The glide of the bar and rows, and the return from a pull past an end
 * or a spin's lead, in frames; 0 with Ultra off. */
unsigned list_ultra_glide_frames(void);
unsigned list_ultra_return_frames(void);
/* Frame `frame` of `frames` of a damped spring from `from`, moving at
 * `velocity` units a frame, to `to`. It overshoots slightly and settles. */
int64_t list_ultra_spring(int64_t from, int64_t to, int64_t velocity,
                          unsigned frame, unsigned frames);
/* The same spring from rest, for the return after a pull. */
int64_t list_ultra_return(int64_t from, int64_t to, unsigned frame,
                          unsigned frames);

/* The glint starts LIST_ULTRA_GLINT_WAIT after the bar settles and
 * crosses it in LIST_ULTRA_GLINT_FRAMES; the last frame draws none. */
#define LIST_ULTRA_GLINT_WAIT 90000 /* us */
#define LIST_ULTRA_GLINT_FRAMES 24
/* Lays the glint's light over `bar`, one colour a row from `rows` from
 * its top, within `clip`, at `frame`. Call after filling the bar and
 * before its text. */
void list_ultra_glint(struct ui_rect clip, struct ui_rect bar,
                      const uint16_t *rows, unsigned frame);

/* The stock marquee's offset into a run of `period` columns, `frame`
 * refreshes after the selection settled, and the frame after `frame` at
 * which it next changes. */
unsigned list_ultra_marquee(unsigned frame, unsigned period);
unsigned list_ultra_marquee_next(unsigned frame, unsigned period);

/* The scroll indicator's strength, 0 to LIST_ULTRA_FULL, `moved` after
 * the selection last moved, when it stood at `from`: it rises to full,
 * holds for `hold`, then fades. */
#define LIST_ULTRA_FULL 256
int list_ultra_indicator(uint32_t moved, int from, uint32_t hold);
/* Whether that strength is rising or fading now. */
bool list_ultra_indicator_moving(uint32_t moved, int from, uint32_t hold);
/* Lays `src`, `width` pixels, over `dst` at `level` of LIST_ULTRA_FULL. */
void list_ultra_mix_row(uint16_t *dst, const uint16_t *src, int width,
                        int level);

#endif
