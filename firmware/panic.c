/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by wavey@wavey.org
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "panic.h"
#include "crumbs.h"
#include "lcd.h"
#include "font.h"
#include "debug.h"
#include "led.h"
#include "power.h"
#include "system.h"
#include "logf.h"
#include "rbversion.h"

#ifdef HAVE_RB_BACKTRACE
#include "gcc_extensions.h"
#include "backtrace.h"
#endif

#if (defined(CPU_MIPS) && (CONFIG_PLATFORM & PLATFORM_NATIVE))
/* TODO: see comment above exception_dump in system-mips.c */
char panic_buf[128];
#else
static char panic_buf[256];
#endif

#define LINECHARS (LCD_WIDTH/SYSFONT_WIDTH) - 2

static volatile bool panic_screen_active;

void panic_screen_enter(void)
{
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
#if defined(CPU_ARM_CLASSIC)
    disable_interrupt(IRQ_FIQ_STATUS);
#else
    set_irq_level(DISABLE_INTERRUPTS);
#endif
#endif
    if (panic_screen_active)
    {
        /* No formatting, drawing, unwinding, PMU or scheduler work here. */
        system_exception_wait();
        while (1);
    }
    panic_screen_active = true;
}

#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
void panicf_f( const char *fmt, ...) USED_ATTR;

/* we wrap panicf() here with naked function to catch SP value */
void __attribute__((naked)) panicf( const char *fmt, ...)
{
    (void)fmt;
    asm volatile ("mov r4, sp \n"
                  "b panicf_f \n"
                 );
    while (1);
}

/*
 * "Dude. This is pretty fucked-up, right here."
 */
void panicf_f( const char *fmt, ...)
{
    int sp;

    asm volatile ("mov %[SP],r4 \n"
                  : [SP] "=r" (sp)
                 );

    int pc = (int)__builtin_return_address(0);
#elif defined(BACKTRACE_MIPSUNWINDER)
void panicf( const char *fmt, ... )
{
    /* NOTE: these are obtained by the backtrace lib */
    const int pc = 0;
    const int sp = 0;
#else
void panicf( const char *fmt, ...)
{
#endif
    va_list ap;

    /* Before the masks: a panic inside a panic, which stays silent, still
     * leaves this for the next boot */
    crumb(CR_PANIC, (uint32_t)(uintptr_t)__builtin_return_address(0));
    panic_screen_enter();

    va_start( ap, fmt );
    vsnprintf( panic_buf, sizeof(panic_buf), fmt, ap );
    va_end( ap );

#ifdef SIMULATOR
    DEBUGF("*PANIC* %s\n", panic_buf);
#endif
#ifdef HAVE_CRUMBS
    crumbs_crash_record(panic_buf, &(struct crumb_crash_context) {
        .kind = CRUMB_CRASH_PANIC,
        .pc = (uintptr_t)__builtin_return_address(0),
#ifdef HAVE_RB_BACKTRACE
        .sp = (uint32_t)sp,
#endif
        .status = get_processor_mode(),
    });
#endif

    lcd_set_viewport(NULL);

    int y = 1;

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, (unsigned char *) "*PANIC* (" RBVERSION ")");
    {
        /* wrap panic line */
        int i, len = strlen(panic_buf);
        for (i=0; i<len; i+=LINECHARS) {
            int end = i + LINECHARS;
            if (end > len) end = len;
            unsigned char c = panic_buf[end];
            panic_buf[end] = 0;
            lcd_puts(1, y++, (unsigned char *)panic_buf+i);
            panic_buf[end] = c;
        }
    }

    /* Commit the first message before any fallible unwinder callbacks. */
#ifdef CPU_ARM_CLASSIC
    lcd_putsf(0, y++, "mode:%02lx", get_processor_mode());
#endif
    lcd_update();
    crumb(CR_PANIC_DRAWN, 0);

#if defined(HAVE_RB_BACKTRACE)
    rb_backtrace(pc, sp, &y);
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf_panic_dump(&y);
#endif

    lcd_update();
#ifndef SIMULATOR
    DEBUGF("%s", panic_buf);
#endif

    /* S5L voltage changes take the I2C mutex. The scheduler cannot release an
     * existing owner after the IRQ mask above, even when this is a SYS panic.
     * Keep the original fault on screen instead of deadlocking or panicking
     * again in mutex_lock from an exception mode. */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && CONFIG_CPU != S5L8702 && CONFIG_CPU != S5L8720
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif

    system_exception_wait(); /* if this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}
