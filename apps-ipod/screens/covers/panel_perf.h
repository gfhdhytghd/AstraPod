/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PANEL_PERF_H
#define PANEL_PERF_H

#include <stdint.h>
#include <stdbool.h>

enum panel_perf_event {
    PP_SELECT, PP_SETTLE, PP_READY, PP_OPEN, PP_QUERY, PP_REMOTE, PP_BLUR,
    PP_FONT, PP_DRAW, PP_DISK, PP_RAM, PP_CANCEL,
    PP_ART_BEGIN, PP_ART_ISSUE, PP_ART_DONE, PP_ART_CANCEL,
    PP_ART_HIT, PP_ART_RAM, PP_ART_DRAW, PP_ART_FRAMES, PP_ART_FALLBACK, PP_ART_END,
};

void panel_perf_init(void);
void panel_perf_art_session(bool active);
void panel_perf_art_drawn(unsigned version, unsigned tier);
void panel_perf_art_frame(bool fallback, bool black);
void panel_perf_record(enum panel_perf_event event, uint32_t elapsed,
                       unsigned value, unsigned detail);

#endif
