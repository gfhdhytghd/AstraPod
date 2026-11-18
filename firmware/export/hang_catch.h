/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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
#ifndef HANG_CATCH_H
#define HANG_CATCH_H

#include <stdbool.h>
#include "config.h"

#if !defined(BOOTLOADER) && (defined(CPU_PP502x) || CONFIG_CPU == S5L8702)
/* Stops the player on a screen of evidence when one thread has held the
 * core for seconds, or the UI thread has sat blocked with keys unread.
 * Started once booting is done, as init itself runs long stretches before
 * any other thread exists. */
void hang_catch_init(void);
/* Before powering off or rebooting, whose last step spins until the
 * hardware acts */
void hang_catch_stop(void);
/* Around work that may run long without yielding, such as loading a theme:
 * true holds the catcher off, false lets it watch again. Calls nest. */
void hang_catch_hold(bool hold);
#if CONFIG_CPU == S5L8702
/* Freeze watch on a timer FIQ, which still runs with IRQs masked: it
 * catches IRQs left off and the UI thread starved (see hang_watch.h). */
#define HAVE_HANG_CATCH_FIQ
#include <stdint.h>
/* The scheduler notes each time the UI thread is switched in */
extern void *hang_ui_thread;
extern long hang_ui_ran;
#ifdef HAVE_RESEARCH_DIAGNOSTICS
extern void *hang_audio_thread;
extern long hang_audio_ran;
void hang_catch_watch_audio(unsigned int id);
#endif
/* From the FIQ, with the interrupted pc and cpsr: nonzero to stop and show
 * the evidence, which the caller does by entering hang_catch_fiq_show on
 * the stack hang_fiq_stack_end gives, never returning */
int hang_catch_fiq(uint32_t pc, uint32_t spsr);
void hang_catch_fiq_show(void);
extern uint32_t *const hang_fiq_stack_end;
/* Target: start or stop the timer FIQ */
void hang_watch_fiq_start(void);
void hang_watch_fiq_stop(void);
#endif
#else
static inline void hang_catch_stop(void) { }
static inline void hang_catch_hold(bool hold) { (void)hold; }
#endif

#endif
