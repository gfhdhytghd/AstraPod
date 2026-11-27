/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INLINE_REMOTE_POWEROFF_H
#define INLINE_REMOTE_POWEROFF_H

#include "button.h"

static inline bool inline_remote_blocks_poweroff(int buttons)
{
    /* The multimedia volume-up code reuses the physical PLAY bit. */
    return (buttons & BUTTON_MULTIMEDIA) != 0;
}

#endif
