/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Robert Keevil
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

#include "system.h"
#include "kernel.h"
#include "piezo.h"
#ifndef BOOTLOADER
#include "piezo-pcm.h"
#endif

static unsigned int duration;
static volatile bool beeping;

void INT_TIMERA(void) ICODE_ATTR;
void INT_TIMERA(void)
{
    /* clear interrupt */
    TACON = TACON;
#ifndef BOOTLOADER
    if (piezo_pcm_active())
    {
        piezo_pcm_irq();
        return;
    }
#endif
    if (!(--duration))
        piezo_stop();
}

static void piezo_start(unsigned short cycles, unsigned short periods)
{
#ifndef SIMULATOR
    duration = periods;
    beeping = 1;
    /* select TA_OUT function on GPIO ports */
    PCON0 = (PCON0 & 0x00ffffff) | 0x53000000;
    /* configure timer for 100 kHz (12 MHz / 4 / 30) */
    TACMD = (1 << 1);   /* TA_CLR */
    TAPRE = 30 - 1;     /* prescaler */
    TACON = (1 << 13) | /* TA_INT1_EN */
            (0 << 12) | /* TA_INT0_EN */
            (0 << 11) | /* TA_START */
            (1 << 8) |  /* TA_CS = ECLK / 4 */
            (1 << 6) |  /* select ECLK (12 MHz) */
            (1 << 4);   /* TA_MODE_SEL = PWM mode */
    TADATA0 = cycles;   /* set interval period */
    TADATA1 = cycles << 1; /* set interval period */
    TACMD = (1 << 1) | (1 << 0);   /* TA_CLR | TA_EN */
#endif
}

void piezo_stop(void)
{
#ifndef SIMULATOR
    int irq = disable_irq_save();
#ifndef BOOTLOADER
    if (piezo_pcm_active())
    {
        restore_irq(irq);
        return;
    }
#endif
    beeping = 0;
    TACON = 7 << 16;    /* disable and acknowledge all Timer A IRQs */
    TACMD = (1 << 1);   /* TA_CLR */
    /* configure GPIO for the lowest power consumption */
    PCON0 = (PCON0 & 0x00ffffff) | 0xee000000;
    restore_irq(irq);
#endif
}

void piezo_clear(void)
{
    piezo_stop();
}

bool piezo_busy(void)
{
    return beeping;
}

void piezo_init(void)
{
    piezo_stop();
}

void piezo_button_beep(bool beep, bool force)
{
    if (force)
        while (beeping)
            yield();

    int irq = disable_irq_save();
#ifndef BOOTLOADER
    if (piezo_pcm_active())
    {
        restore_irq(irq);
        return;
    }
#endif
    if (!beeping)
    {
        if (beep)
            piezo_start(22, 457);
        else
            piezo_start(40, 4);
    }
    restore_irq(irq);
}

/* A square wave of frequency Hz for duration ms, replacing any beep that is
 * sounding. Timer A counts at 100 kHz and interrupts once per wave. */
void piezo_play(unsigned int frequency, unsigned int duration)
{
    int irq = disable_irq_save();
#ifndef BOOTLOADER
    if (piezo_pcm_active())
    {
        restore_irq(irq);
        return;
    }
#endif
    piezo_stop();
    if (frequency >= 2 && duration != 0)
    {
        unsigned int cycles = (50000 + frequency / 2) / frequency;
        unsigned int periods = (duration * frequency + 999) / 1000;
        piezo_start(MAX(cycles, 1u), MIN(periods, 0xffffu));
    }
    restore_irq(irq);
}

#ifdef BOOTLOADER
void piezo_tone(uint32_t period /*uS*/, int32_t duration /*ms*/)
{
    int32_t stop = USEC_TIMER + duration*1000;
    uint32_t level = 0;

    while ((int32_t)USEC_TIMER - stop < 0)
    {
        level ^= 1;
        GPIOCMD = 0x0060e | level;
        udelay(period >> 1);
    }

    GPIOCMD = 0x0060e;
}

void piezo_seq(uint16_t *seq)
{
    uint16_t period;

    while ((period = *seq++) != 0)
    {
        piezo_tone(period, *seq++);
        udelay(*seq++ * 1000);
    }
}
#endif
