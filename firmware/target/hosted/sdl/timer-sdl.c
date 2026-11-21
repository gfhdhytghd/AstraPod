/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* Copyright (C) 2005 Kévin Ferrare
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

#include "timer.h"
#include <SDL_timer.h>
#ifdef ASTRAPOD_ROCK_COMPAT
#include <SDL_atomic.h>
#include <stdint.h>

static SDL_SpinLock ticks_lock;
static SDL_TimerID ticks_id;
static uintptr_t ticks_generation;
static unsigned int pending_ticks;
#endif

static int timer_prio = -1;
void (*global_timer_callback)(void);
SDL_TimerID timerId;

Uint32 SDL_timer_callback(Uint32 interval, void *param){
    (void)param;
    global_timer_callback();
    return(interval);
}

#define cycles_to_miliseconds(cycles) \
    ((int)((1000*cycles)/TIMER_FREQ))

bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void))
{
    (void)unregister_callback;
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_id)
        return false;
#endif
    if (reg_prio <= timer_prio || cycles == 0)
        return false;
    timer_prio=reg_prio;
    global_timer_callback=timer_callback;
    timerId=SDL_AddTimer(cycles_to_miliseconds(cycles), SDL_timer_callback, 0);
    return true;
}

bool timer_set_period(long cycles)
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_id)
        return false;
#endif
    SDL_RemoveTimer (timerId);
    timerId=SDL_AddTimer(cycles_to_miliseconds(cycles), SDL_timer_callback, 0);
    return true;
}

void timer_unregister(void)
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (ticks_id)
        return;
#endif
    SDL_RemoveTimer (timerId);
    timer_prio = -1;
}

#ifdef ASTRAPOD_ROCK_COMPAT
static Uint32 count_tick(Uint32 interval, void *generation)
{
    SDL_AtomicLock(&ticks_lock);
    bool active = ticks_id && (uintptr_t)generation == ticks_generation;
    if (active)
        ++pending_ticks;
    SDL_AtomicUnlock(&ticks_lock);
    return active ? interval : 0;
}

bool timer_reserve_ticks(long cycles)
{
    /* Round up to SDL's millisecond resolution. */
    if (ticks_id || timer_prio != -1 || cycles <= 0)
        return false;

    const unsigned long cycles_per_ms = TIMER_FREQ / 1000;
    Uint32 interval = ((unsigned long)cycles - 1) / cycles_per_ms + 1;
    SDL_AtomicLock(&ticks_lock);
    ticks_id = SDL_AddTimer(interval, count_tick, (void *)++ticks_generation);
    SDL_AtomicUnlock(&ticks_lock);
    return ticks_id != 0;
}

unsigned int timer_take_ticks(void)
{
    SDL_AtomicLock(&ticks_lock);
    unsigned int ticks = pending_ticks;
    pending_ticks = 0;
    SDL_AtomicUnlock(&ticks_lock);
    return ticks;
}

void timer_release_ticks(void)
{
    SDL_AtomicLock(&ticks_lock);
    SDL_TimerID id = ticks_id;
    ticks_id = 0;
    pending_ticks = 0;
    SDL_AtomicUnlock(&ticks_lock);
    /* A late callback from this timer sees an inactive or newer generation. */
    if (id)
        SDL_RemoveTimer(id);
}
#endif
