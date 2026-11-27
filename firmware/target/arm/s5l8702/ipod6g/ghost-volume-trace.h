/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GHOST_VOLUME_TRACE_H
#define GHOST_VOLUME_TRACE_H

#include <stdint.h>
#include "inline-remote-poweroff.h"

enum ghost_volume_event {
    GHOST_BUTTON_SOURCES,
    GHOST_MIKEY_VOLUME_EDGE,
    GHOST_VOLUME_EVENT,
    GHOST_VOLUME_SET,
    GHOST_PLAY_POWEROFF,
    GHOST_INLINE_POWEROFF_BLOCKED,
    GHOST_BATTERY_POWEROFF_DISK,
    GHOST_BATTERY_POWEROFF,
    GHOST_IDLE_POWEROFF,
    GHOST_SLEEP_POWEROFF,
    GHOST_POWEROFF_REQUEST,
    GHOST_REBOOT_REQUEST,
    GHOST_CLEAN_SHUTDOWN,
    GHOST_FORCED_POWEROFF,
    GHOST_VOLUME_EVENT_COUNT
};

/* IRQ-safe. The USB/Accessory log writer drains this independent ring. */
void ghost_volume_trace(enum ghost_volume_event event,
                        uint32_t a, uint32_t b, uint32_t c);
void ghost_volume_trace_flush(void);

static inline void ghost_volume_button_sources(int wheel, int serial, int mikey)
{
    static int prev_wheel, prev_serial, prev_mikey;

    if (wheel == prev_wheel && serial == prev_serial && mikey == prev_mikey)
        return;
    prev_wheel = wheel;
    prev_serial = serial;
    prev_mikey = mikey;
    ghost_volume_trace(GHOST_BUTTON_SOURCES, wheel, serial, mikey);
}

static inline void ghost_volume_poweroff_key(int buttons, int repeats)
{
    if (!(buttons & POWEROFF_BUTTON))
        return;
    enum ghost_volume_event event = inline_remote_blocks_poweroff(buttons) ?
        GHOST_INLINE_POWEROFF_BLOCKED : GHOST_PLAY_POWEROFF;
    ghost_volume_trace(event, buttons, repeats, 0);
}

#endif
