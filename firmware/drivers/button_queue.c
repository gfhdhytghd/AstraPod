/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
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
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "button_chord.h"
#include <limits.h>
#ifdef HAVE_SDL
#include "SDL.h"
#if SDL_MAJOR_VERSION > 1
#include "window-sdl.h"
#endif
#endif

static struct event_queue button_queue SHAREDBSS_ATTR;
static intptr_t button_data; /* data value from last message dequeued */

#ifdef HAVE_BUTTON_FRAME_DEADLINE
#include "button-frame-deadline.inc"
#else
#define button_frame_wait(ev, ticks) queue_wait_w_tmo(&button_queue, ev, ticks)
#define button_discard_frame_wake() ((void)0)
#endif

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static bool button_boosted = false;
static long button_unboost_tick;
#define BUTTON_UNBOOST_TMO HZ

static void button_boost(bool state)
{
    if (state)
    {
        /* update the unboost time each button_boost(true) call */
        button_unboost_tick = current_tick + BUTTON_UNBOOST_TMO;

        if (!button_boosted)
        {
            button_boosted = true;
            cpu_boost(true);
        }
    }
    else if (button_boosted && TIME_AFTER(current_tick, button_unboost_tick))
    {
        button_boosted = false;
        cpu_boost(false);
    }
}

static void button_queue_wait(struct queue_event *evp, int timeout)
{
    /* Loop once after wait time if boosted in order to unboost and wait the
       full remaining time */
    do
    {
        int ticks = timeout;

        if (ticks == TIMEOUT_NOBLOCK)
            ;
        else if (ticks > 0)
        {
            if (button_boosted && ticks > BUTTON_UNBOOST_TMO)
                ticks = BUTTON_UNBOOST_TMO;

            timeout -= ticks;
        }
        else if (button_boosted) /* TIMEOUT_BLOCK (ticks < 0) */
        {
            ticks = BUTTON_UNBOOST_TMO;
        }

        button_frame_wait(evp, ticks);
        if (evp->id != SYS_TIMEOUT)
        {
            /* GUI boost build gets immediate kick, otherwise at least 3
               messages had to be there */
        #ifndef HAVE_GUI_BOOST
            if (queue_count(&button_queue) >= 2)
        #endif
                button_boost(true);

            break;
        }
        button_boost(false);
    }
    while (timeout);
}
#else /* ndef HAVE_ADJUSTABLE_CPU_FREQ */
static inline void button_queue_wait(struct queue_event *evp, int timeout)
{
#if defined(__APPLE__) && (CONFIG_PLATFORM & PLATFORM_SDL)
    unsigned long initial_tick = current_tick;
    unsigned long curr_tick, remaining;
    while(true)
    {
        SDL_PumpEvents();
        queue_wait_w_tmo(&button_queue, evp, TIMEOUT_NOBLOCK);
        if (evp->id != SYS_TIMEOUT || timeout == TIMEOUT_NOBLOCK)
            return;
        else if (timeout == TIMEOUT_BLOCK)
            sleep(HZ/60);
        else
        {
            curr_tick = current_tick;
            if (!TIME_AFTER(initial_tick + timeout, curr_tick))
                return;
            remaining = ((initial_tick + timeout) - curr_tick);
            sleep(remaining < HZ/60 ? remaining : HZ/60);
        }
    }
#else
    queue_wait_w_tmo(&button_queue, evp, timeout);
#if defined(HAVE_SDL) && (SDL_MAJOR_VERSION > 1)
    sdl_window_adjust(); /* Window may have been resized */
#endif
#endif
}
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

void button_queue_post(long id, intptr_t data)
{
    int oldlevel = disable_irq_save();
    button_discard_frame_wake();
    if (queue_full(&button_queue))
    {
#if defined(HAVE_EXTENDED_MESSAGING_AND_NAME) && \
    defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)
        static const long wheel[][2] = {
            { BUTTON_SCROLL_FWD, BUTTON_SCROLL_FWD },
            { BUTTON_SCROLL_BACK, BUTTON_SCROLL_BACK },
        };
        queue_peek_ex(&button_queue, NULL,
                      QPEEK_REMOVE_EVENTS | (ARRAYLEN(wheel) - 1), wheel);
#endif
        if (queue_full(&button_queue))
        {
            restore_irq(oldlevel);
            return;
        }
    }
    queue_post(&button_queue, id, data);
    restore_irq(oldlevel);
}

void button_queue_post_remove_head(long id, intptr_t data)
{
    queue_remove_from_head(&button_queue, id);
    queue_post(&button_queue, id, data);
}

bool button_queue_try_post(long button, int data)
{
    int oldlevel = disable_irq_save();
    button_discard_frame_wake();
#ifdef HAVE_TOUCHSCREEN
    /* one can swipe over the scren very quickly,
     * for this to work we want to forget about old presses and
     * only respect the very latest ones */
    const bool force_post = true;
#else
    /* Only post events if the queue is empty,
     * to avoid afterscroll effects.
     * i.e. don't post new buttons if previous ones haven't been
     * processed yet - but always post releases */
    const bool force_post = button & BUTTON_REL;
#endif

    if (!queue_empty(&button_queue))
    {
        if (force_post)
            queue_remove_from_head(&button_queue, button);
        else
            goto not_posted;
    }

    if (queue_full(&button_queue))
    {
#if defined(HAVE_EXTENDED_MESSAGING_AND_NAME) && \
    defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)
        /* A burst of wheel ticks must not crowd out a key release. Drop
         * one pending wheel tick without removing system events. */
        static const long wheel[][2] = {
            { BUTTON_SCROLL_FWD, BUTTON_SCROLL_FWD },
            { BUTTON_SCROLL_BACK, BUTTON_SCROLL_BACK },
        };
        if (!queue_peek_ex(&button_queue, NULL,
                           QPEEK_REMOVE_EVENTS | (ARRAYLEN(wheel) - 1), wheel))
#endif
            goto not_posted;
    }
    queue_post(&button_queue, button, data);
    restore_irq(oldlevel);
    return true;

not_posted:
    restore_irq(oldlevel);
    return false;
}

int button_queue_count(void)
{
    return queue_count(&button_queue);
}

bool button_queue_empty(void)
{
    return queue_empty(&button_queue);
}

bool button_queue_full(void)
{
    return queue_full(&button_queue);
}

#if (defined(HAVE_USB_PODLINK) || (defined(SIMULATOR) && defined(IPOD_6G))) && !defined(BOOTLOADER)
bool button_queue_wait_pending(int ticks)
{
    const bool forever = ticks == TIMEOUT_BLOCK;
    long deadline = current_tick + MAX(ticks, 0);
    do {
#ifdef HAVE_EXTENDED_MESSAGING_AND_NAME
        /* A release from the gesture which closed the sheet still belongs
         * to the action engine. Leave it queued, but look past it for a
         * new key or a system takeover. The button masks are ordered. */
        static const long actions[][2] = {
            {LONG_MIN, -1},
            {BUTTON_NONE + 1, BUTTON_REL - 1},
            {BUTTON_REPEAT, BUTTON_REPEAT | (BUTTON_REL - 1)},
            {BUTTON_TOUCHSCREEN, BUTTON_TOUCHSCREEN | (BUTTON_REL - 1)},
            {BUTTON_TOUCHSCREEN | BUTTON_REPEAT,
             BUTTON_TOUCHSCREEN | BUTTON_REPEAT | (BUTTON_REL - 1)},
            {BUTTON_MULTIMEDIA, BUTTON_REDRAW - 1},
        };
        if (queue_peek_ex(&button_queue, NULL, ARRAYLEN(actions) - 1, actions))
            return true;
        queue_remove_from_head(&button_queue, BUTTON_NONE);
        queue_remove_from_head(&button_queue, BUTTON_REDRAW);
        if (ticks == TIMEOUT_NOBLOCK)
            return false;
        if (!queue_empty(&button_queue))
            sleep(1); /* Queue waits wake immediately while a release is pending. */
        else
            queue_wait_w_tmo(&button_queue, NULL, ticks);
#else
        queue_wait_w_tmo(&button_queue, NULL, ticks);
        return !queue_empty(&button_queue);
#endif
        if (!forever)
            ticks = MAX(0, deadline - current_tick);
    } while (true);
}
#endif

void button_clear_queue(void)
{
    queue_clear(&button_queue);
}

/* clears anything but release and sysevents */
void button_clear_pressed(void)
{
    long button;
    for (int count = queue_count(&button_queue); count > 0; count--)
    {
        button = button_get(false);
        if (button & (BUTTON_REL | SYS_EVENT))
        {
            button_queue_post(button, button_data);
        }
    }
}

#ifdef HAVE_WHEEL_WPS_CHORD
static long (*ui_filter)(long);
void button_set_ui_filter(long (*filter)(long)) { ui_filter = filter; }
#endif
#if defined(HAVE_USB_PODLINK) && !defined(BOOTLOADER)
static int (*ui_timeout)(int);
void button_set_ui_timeout(int (*timeout)(int)) { ui_timeout = timeout; }
#endif

long button_get_w_tmo(int ticks)
{
    struct queue_event ev;
#if defined(HAVE_USB_PODLINK) && !defined(BOOTLOADER)
    if (ui_timeout) ticks = ui_timeout(ticks);
#endif
    button_queue_wait(&ev, ticks);

    if (ev.id == SYS_TIMEOUT)
        ev.id = BUTTON_NONE;
    else
        button_data = ev.data;

#ifdef HAVE_WHEEL_WPS_CHORD
    if (ui_filter) return ui_filter(ev.id);
#endif
    return ev.id;
}

long button_get(bool block)
{
    return button_get_w_tmo(block ? TIMEOUT_BLOCK : TIMEOUT_NOBLOCK);
}

intptr_t button_get_data(void)
{
    return button_data;
}

void INIT_ATTR button_queue_init(void)
{
    queue_init(&button_queue, true);
}
