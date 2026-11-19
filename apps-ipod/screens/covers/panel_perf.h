/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PANEL_PERF_H
#define PANEL_PERF_H

#include <stdint.h>

enum panel_perf_event {
    PP_SELECT, PP_SETTLE, PP_READY, PP_OPEN, PP_QUERY, PP_REMOTE, PP_BLUR,
    PP_FONT, PP_DRAW, PP_DISK, PP_RAM, PP_CANCEL,
};

void panel_perf_init(void);
void panel_perf_record(enum panel_perf_event event, uint32_t elapsed,
                       unsigned value, unsigned detail);

#endif
