/* Stable, bounded message layout. GPL version 2 or later. */
#ifndef DETAIL_LAYOUT_H
#define DETAIL_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>
#include "herdremote_model.h"
#define DETAIL_LAYOUT_BYTES 4096
#define DETAIL_LAYOUT_LINES 128
struct detail_layout {
    char text[DETAIL_LAYOUT_BYTES], wire[HR_DETAIL_SAY];
    char pane[16], identity[9], steps[HR_DETAIL_STEPS][HR_DETAIL_TEXT];
    uint16_t start[DETAIL_LAYOUT_LINES], end[DETAIL_LAYOUT_LINES];
    int count, used, width;
};
/* Measure a complete candidate line so the caller can apply pair kerning.
 * Never reflow a closed line. Freeze keeps the text and tool rows in place. */
void detail_layout_update(struct detail_layout *c, const char *pane,
    const char *identity, const char *text, const struct hr_detail *detail,
    int width, bool freeze, int (*measure)(const char *, int, void *), void *ctx);
#endif
