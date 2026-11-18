/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Thom Johansen
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
#include "crumbs.h"
#include <stdio.h>
#include "lcd.h"
#include "font.h"
#include "gcc_extensions.h"
#include "panic.h"
#include "research.h"

#include <get_sp.h>
#include <backtrace.h>
#ifndef BOOTLOADER
const char *thread_current_name(void);
#endif

static const char* const uiename[] = {
    "Undefined instruction",
    "Prefetch abort",
    "Data abort",
    "Divide by zero",
    "SWI"
};

#ifdef HAVE_RESEARCH_DIAGNOSTICS
void NORETURN_ATTR USED_ATTR research_exception(unsigned int pc, unsigned int num,
                                                const uint32_t *regs)
{
    struct research_fault fault = { .pc = pc, .regs = regs };
    unsigned int here;
    disable_interrupt(IRQ_FIQ_STATUS);
    asm volatile("mrs %0, spsr\n\tmrs %1, cpsr"
                 : "=r"(fault.cpsr), "=r"(here));
    unsigned int bank = fault.cpsr & 0x1f;
    if (bank == 0x10)
        bank = 0x1f;
    if (bank != (here & 0x1f) &&
        (bank == 0x1f || bank == 0x13 || bank == 0x12))
    {
        asm volatile("msr cpsr_c, %3\n\tmov %0, sp\n\tmov %1, lr\n\tmsr cpsr_c, %2"
                     : "=&r"(fault.sp), "=&r"(fault.lr)
                     : "r"(here | 0xc0), "r"(bank | 0xc0));
    }
    if (num != 0)
        asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(fault.fsr));
    if (num == 2)
        asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(fault.far));
    else if (num == 1)
        fault.far = pc;
    research_capture(RESEARCH_UNDEFINED + num, &fault, NULL);
}
#define RESEARCH_EXCEPTION_SAVE "sub sp, sp, #56\n\tstmia sp, {r0-r12}\n\t"
#define RESEARCH_EXCEPTION_CALL "mov r2, sp\n\tb research_exception\n\t"
#else
#define RESEARCH_EXCEPTION_SAVE ""
#define RESEARCH_EXCEPTION_CALL "b UIE\n\t"
#endif

void __attribute__((weak,naked)) data_abort_handler(void)
{
    asm volatile(
        RESEARCH_EXCEPTION_SAVE
        "sub    r0, lr, #8  \n"
        "mov    r1, #2      \n"
        RESEARCH_EXCEPTION_CALL
        );
}

void __attribute__((weak,naked)) software_int_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) reserved_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) prefetch_abort_handler(void)
{
    asm volatile(
        RESEARCH_EXCEPTION_SAVE
        "sub    r0, lr, #4  \n"
        "mov    r1, #1      \n"
        RESEARCH_EXCEPTION_CALL
        );
}

void __attribute__((weak,naked)) undef_instr_handler(void)
{
    asm volatile(
        RESEARCH_EXCEPTION_SAVE
        "sub    r0, lr, #4    \n"
#if defined(USE_THUMB) || defined(HAVE_RESEARCH_DIAGNOSTICS)
        "mrs    r1, spsr      \n"
        "tst    r1, #(1 << 5) \n" // T bit set ?
        "subne  r0, lr, #2    \n" // if yes, offset to THUMB instruction
#endif
        "mov    r1, #0        \n"
        RESEARCH_EXCEPTION_CALL
        );
}

/* Unexpected Interrupt or Exception handler. Currently only deals with
   exceptions, but will deal with interrupts later.
 */
void NORETURN_ATTR UIE(unsigned int pc, unsigned int num)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;
#ifndef BOOTLOADER
    unsigned int exception_mode = 0, saved_status = 0;
    unsigned int user_context[2] = {0, 0}, saved_sp = 0, saved_lr = 0;
    asm volatile("mrs %0, cpsr" : "=r"(exception_mode));
    bool banked = (exception_mode & 0x1f) == 0x17 ||
                  (exception_mode & 0x1f) == 0x1b;
    if (banked)
    {
        asm volatile("mrs %0, spsr" : "=r"(saved_status));
        /* ABT has its own SP/LR. Read the interrupted SYS/USR bank separately. */
        asm volatile("stmia %0, {r13, r14}^\n\tnop"
                     : : "r"(user_context) : "memory");
        /* Threads run in SVC: briefly enter the interrupted mode (IRQ/FIQ
         * masked) to read its banked SP/LR, then return to this mode. */
        if ((saved_status & 0x1f) != 0x10 && (saved_status & 0x1f) != 0x1f)
        {
            unsigned int here, there;
            asm volatile(
                "mrs   %2, cpsr        \n"
                "bic   %3, %2, #0x1f   \n"
                "orr   %3, %3, %4      \n"
                "orr   %3, %3, #0xc0   \n"
                "msr   cpsr_c, %3      \n"
                "mov   %0, sp          \n"
                "mov   %1, lr          \n"
                "msr   cpsr_c, %2      \n"
                : "=&r"(saved_sp), "=&r"(saved_lr), "=&r"(here), "=&r"(there)
                : "r"(saved_status & 0x1f));
        }
        else
        {
            saved_sp = user_context[0];
            saved_lr = user_context[1];
        }
    }
    panic_screen_enter();
    /* An exception can interrupt a screen whose viewport lives on its stack. */
    lcd_set_viewport(NULL);
#else
    panic_screen_enter();
#endif

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
#ifdef BOOTLOADER
    lcd_set_viewport(NULL);
#endif
    lcd_clear_display();
    lcd_putsf(0, line++, "%s at %08x" IF_COP(" (%d)"), uiename[num], pc IF_COP(, CURRENT_CORE));

#if !defined(CPU_ARM7TDMI) && (CONFIG_CPU != RK27XX) /* arm7tdmi has no MPU/MMU */
    if(num == 1 || num == 2) /* prefetch / data abort */
    {
        register unsigned status;

#if ARM_ARCH >= 6
        /* ARMv6 has 2 different registers for prefetch & data aborts */
        if(num == 1)    /* instruction prefetch abort */
            asm volatile( "mrc p15, 0, %0, c5, c0, 1\n" : "=r"(status));
        else
#endif
            asm volatile( "mrc p15, 0, %0, c5, c0, 0\n" : "=r"(status));

        lcd_putsf(0, line++, "FSR 0x%x", status);

        unsigned int domain = (status >> 4) & 0xf;
        unsigned int fault = status & 0xf;
#if ARM_ARCH >= 6
        fault |= (status & (1<<10)) >> 6; /* fault is 5 bits on armv6 */
#endif
        lcd_putsf(0, line++, "(domain %d, fault %d)", domain, fault);

        if(num == 2) /* data abort */
        {
            register unsigned address;
            /* read FAR (fault address register) */
            asm volatile( "mrc p15, 0, %0, c6, c0\n" : "=r"(address));
            lcd_putsf(0, line++, "address 0x%8x", address);
#if ARM_ARCH >= 6
            lcd_putsf(0, line++, (status & (1<<11)) ? "(write)" : "(read)");
#endif
        }
    }   /* num == 1 || num == 2 // prefetch/data abort */
#endif /* !defined(CPU_ARM7TDMI */

#ifndef BOOTLOADER
    if (banked)
    {
        extern char _textstart[], _textend[], _end[];
        lcd_putsf(0, line++, "mode:%02x thr:%.20s", saved_status & 0x1f,
                  thread_current_name());
        lcd_putsf(0, line++, "sp:%08x lr:%08x", saved_sp, saved_lr);
        /* Candidate return addresses on the interrupted stack. */
        unsigned int found = 0, text[12];
        const unsigned int *word = (const unsigned int *)(saved_sp & ~3u);
        bool dram = saved_sp >= 0x08000000u && saved_sp < (uintptr_t)_end;
#ifdef HAVE_CRUMBS     /* FU51: no stacks in page 0 */
        bool iram = saved_sp >= 0x1000u && saved_sp < 0x10000u;
#else
        bool iram = saved_sp >= 0x60u && saved_sp < 0x10000u;
#endif
        for (unsigned int i = 0; (dram || iram) && i < 384 && found < 12; i++)
        {
            if ((uintptr_t)(word + i) >= (dram ? (uintptr_t)_end : 0x10000u))
                break;
            unsigned int value = word[i];
            if (value >= (uintptr_t)_textstart && value < (uintptr_t)_textend)
                text[found++] = value;
        }
        for (unsigned int i = 0; i < found; i += 3)
            lcd_putsf(0, line++, "%08x %08x %08x", text[i],
                      i + 1 < found ? text[i + 1] : 0,
                      i + 2 < found ? text[i + 2] : 0);
    }
#endif
    lcd_update(); /* Keep the first exception visible if unwinding faults. */
#ifdef HAVE_RB_BACKTRACE
    if (!triggered)
    {
        triggered = true;
        rb_backtrace(pc, __get_sp(), &line);
    }
#else
    (void)triggered;
#endif

    lcd_update();

    disable_interrupt(IRQ_FIQ_STATUS);

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}

/* Needs to be here or gcc won't find it */
void __attribute__((naked)) __div0(void)
{
    asm volatile (
        "ldr    r0, [sp]    \r\n"
        "sub    r0, r0, #4  \r\n"
        "mov    r1, #3      \r\n"
        "b      UIE         \r\n"
    );
}
