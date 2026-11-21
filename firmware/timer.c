/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* Copyright (C) 2005 Jens Arnold
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
* KIND, either express or implied.
*
****************************************************************************/

#include <stdbool.h>
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "timer.h"
#include "logf.h"

static int timer_prio = -1;
#ifdef ASTRAPOD_ROCK_COMPAT
static bool ticks_reserved;
static volatile unsigned int pending_ticks;
#endif
void SHAREDBSS_ATTR (*pfn_timer)(void) = NULL;      /* timer callback */
void SHAREDBSS_ATTR (*pfn_unregister)(void) = NULL; /* unregister callback */

/* Register a user timer, called every <cycles> TIMER_FREQ cycles */
bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void)
                    IF_COP(, int core))
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_reserved)
        return false;
#endif
    if (reg_prio <= timer_prio || cycles == 0)
        return false;

    if (!timer_set(cycles, true))
        return false;

    pfn_timer = timer_callback;
    /* NOTE: if unreg cb is defined you are in charge of calling timer_unregister() */
    pfn_unregister = unregister_callback;
    timer_prio = reg_prio;

    return timer_start(IF_COP(core));
}

bool timer_set_period(long cycles)
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_reserved)
        return false;
#endif
    return timer_set(cycles, false);
}

/* NOTE: unregister callbacks are not called by timer_unregister()
* the unregister_callback only gets called when your timer gets 
* overwritten by a lower priority timer using timer_register() */
void timer_unregister(void)
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_reserved)
        return;
#endif
    timer_stop();

    pfn_timer = NULL;
    pfn_unregister = NULL;
    timer_prio = -1;
}

#ifdef ASTRAPOD_ROCK_COMPAT
static void count_tick(void)
{
    ++pending_ticks;
}

bool timer_reserve_ticks(long cycles)
{
    int irq = disable_irq_save();
    bool ok = false;
    if (!ticks_reserved && timer_prio == -1 && cycles > 0 &&
        timer_set(cycles, true))
    {
        pending_ticks = 0;
        pfn_timer = count_tick;
        ticks_reserved = timer_start();
        if (!ticks_reserved)
        {
            timer_stop();
            pfn_timer = NULL;
        }
        ok = ticks_reserved;
    }
    restore_irq(irq);
    return ok;
}

unsigned int timer_take_ticks(void)
{
    int irq = disable_irq_save();
    unsigned int ticks = pending_ticks;
    pending_ticks = 0;
    restore_irq(irq);
    return ticks;
}

void timer_release_ticks(void)
{
    int irq = disable_irq_save();
    if (ticks_reserved)
    {
        timer_stop();
        pfn_timer = NULL;
        pending_ticks = 0;
        ticks_reserved = false;
    }
    restore_irq(irq);
}
#endif

