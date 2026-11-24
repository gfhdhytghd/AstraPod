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

/* Hang catcher. Threads are cooperative, so one that never gives up the
 * core starves every other: no sound, no backlight, no buttons, and no
 * panic. The scheduler handles timed waits each time it switches, so a
 * timed wait several seconds overdue says nothing has switched for that
 * long. The tick then sends that thread to a screen naming it, with a
 * backtrace from the instruction the tick interrupted. A hang with
 * interrupts off stops the tick too, and is not caught: a frozen player with
 * no such screen is evidence of that.
 *
 * A deadlock lets the core switch freely, so the tick watches for it apart:
 * the UI thread blocked with no time limit while key presses wait unread.
 * A wait for a key ends at the first one, so this is a wait on another
 * thread or a lock that has not come back, and the screen lists what every
 * thread was doing. */

#include <stdio.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "power.h"
#include "rbversion.h"
#include "backtrace.h"
#include "safe_read.h"
#include "string-extra.h"
#include "thread-internal.h"
#include "usb_log.h"
#include "button.h"
#include "hang_catch.h"
#include "panic.h"
#include "crumbs.h"

#define HANG_SECS   10
/* Longer than the core check: the UI thread can wait this long on the
 * audio thread through slow disk work, where a thread that never yields
 * has no excuse at all. Long work done on the UI thread itself, such as a
 * playlist being built, keeps it running and never counts. */
#define STUCK_SECS  20

static const uint32_t *irq_stack_top;

/* Whether the word before a is a call: BL, BX or BLX by register, or a load
 * into pc after a "mov lr, pc" */
static bool after_call(uint32_t a)
{
    uint32_t i;

    /* past the exception vectors, whose loads into pc would pass */
    if ((a & 3) || a < 0x100 || !safe_read32((uint32_t *)(a - 4), &i))
        return false;
    return (i & 0x0f000000) == 0x0b000000 ||
           (i & 0x0fffffd0) == 0x012fff10 ||
           (i & 0x0c10f000) == 0x0410f000;
}

/* The unwinder steps forward to a function's return, which a loop that
 * never ends never reaches. The words on the stack that follow a call are
 * the callers, with some stale ones among them, from the innermost. */
#define STACK_CALLS 32
static int stack_scan(uint32_t sp, uint32_t top, uint32_t *found, int most)
{
    uint32_t w;
    int n = 0;

    for (; sp < top && n < most; sp += 4)
        if (safe_read32((uint32_t *)sp, &w) && after_call(w))
            found[n++] = w;
    return n;
}

/* Where the stuck thread was, taken by the tick */
static const struct thread_entry *hang_thread;
static uint32_t hang_pc, hang_sp, hang_lr;

/* The UI thread, which starts the catcher; when it was first seen blocked
 * with keys unread, or 0; and whether that, not the core, is the report */
static const struct thread_entry *ui_thread;
static long stuck_since;
static bool hang_stuck;

/* How many holds are open, and when the last one closed: the core check
 * counts from then, as the scheduler may not have run since */
static volatile int holds;
static long hold_end;

static char state_letter(unsigned char state)
{
    switch (state)
    {
        case STATE_RUNNING:       return 'R';
        case STATE_BLOCKED:       return 'B';
        case STATE_SLEEPING:      return 'S';
        case STATE_BLOCKED_W_TMO: return 'T';
        case STATE_FROZEN:        return 'F';
        default:                  return '?';
    }
}

static void name8(const struct thread_entry *t, char name[8])
{
    memset(name, 0, 8);
    strlcpy(name, t && t->name ? t->name : "?", 8);
}

/* Every live thread: its name and state, the return address its switch
 * saved -- inside the call it is waiting in -- and the first caller found
 * on its stack. The thread the tick interrupted is drawing this, so its
 * saved registers are stale; it is marked. */
static void stuck_log(void)
{
    char name[8];

    usb_log(USB_LOG_HANG, 3, 0, STUCK_SECS, 0);
    for (unsigned i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *t = __thread_slot_entry(i);
        uint32_t call = 0;

        if (t->state == STATE_KILLED)
            continue;
        stack_scan(t->context.sp, (uint32_t)t->stack + t->stack_size, &call,
                   1);
        name8(t, name);
        const uint8_t *c = (const uint8_t *)name;
        usb_log(USB_LOG_HANG, 4, state_letter(t->state),
                (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
                (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
        usb_log(USB_LOG_HANG, 5, t == hang_thread, t->context.lr, call);
    }
    usb_log_sync();
}

static void stuck_draw(unsigned int *y)
{
    lcd_puts(1, (*y)++, "thread  s lr       calls");
    for (unsigned i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *t = __thread_slot_entry(i);
        uint32_t calls[2] = { 0, 0 };
        char name[8];

        if (t->state == STATE_KILLED)
            continue;
        stack_scan(t->context.sp, (uint32_t)t->stack + t->stack_size,
                   calls, 2);
        name8(t, name);
        lcd_putsf(1, (*y)++, "%-7.7s %c %08lx %08lx %08lx", name,
                  t == hang_thread ? '*' : state_letter(t->state),
                  (unsigned long)t->context.lr, (unsigned long)calls[0],
                  (unsigned long)calls[1]);
    }
}

/* The report goes to the USB log first, and waits up to three seconds for
 * the log's writer: once interrupts are off for the screen, nothing else
 * runs. */
static void hang_log(const struct thread_entry *t, const uint32_t *calls,
                     int n)
{
    char name[8] = "?";

    if (t && t->name)
        strlcpy(name, t->name, sizeof name);
    const uint8_t *c = (const uint8_t *)name;

    usb_log(USB_LOG_HANG, 0, 0,
            (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
            (uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
    usb_log(USB_LOG_HANG, 1, 0, hang_pc, hang_lr);
    for (int i = 0; i < n; i += 2)
        usb_log(USB_LOG_HANG, 2, i / 2, calls[i],
                i + 1 < n ? calls[i + 1] : 0);
    usb_log_sync();
}

/* Entered by the stuck thread itself, as the tick returned it here rather
 * than to its loop: the report waits on the log's writer, which an
 * interrupt cannot. */
static void NORETURN_ATTR hang_show(void)
{
    const struct thread_entry *t = hang_thread;
    uint32_t calls[STACK_CALLS];
    unsigned int y = 1;
    int n = t ? stack_scan(hang_sp, (uint32_t)t->stack + t->stack_size,
                           calls, STACK_CALLS) : 0;

    crumb(CR_HANG, hang_stuck ? 0 : hang_pc);
    if (hang_stuck)
        stuck_log();
    else
        hang_log(t, calls, n);
    panic_screen_enter();
    char message[80];
    if (hang_stuck)
        snprintf(message, sizeof(message), "%s blocked %ds, keys unread",
                 ui_thread && ui_thread->name ? ui_thread->name : "?", STUCK_SECS);
    else
        snprintf(message, sizeof(message), "%s held the core %ds",
                 t && t->name ? t->name : "?", HANG_SECS);
#ifdef HAVE_CRUMBS
    crumbs_crash_record(message, &(struct crumb_crash_context) {
        .kind = CRUMB_CRASH_HANG, .pc = hang_pc, .sp = hang_sp, .lr = hang_lr,
        .status = get_processor_mode(),
    });
#endif
    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, "*HANG* (" RBVERSION ")");
    lcd_puts(1, y++, message);
    if (hang_stuck)
        stuck_draw(&y);
    else
    {
        lcd_putsf(1, y++, "lr:%08lx", (unsigned long)hang_lr);
        lcd_update();
        crumb(CR_HANG_DRAWN, 0);
        rb_backtrace(hang_pc, hang_sp, &y);
        lcd_puts(1, y++, "stack:");
        for (int i = 0; i < n; i += 4)
            lcd_putsf(1, y++, "%08lx %08lx %08lx %08lx",
                      (unsigned long)calls[i],
                      (unsigned long)(i + 1 < n ? calls[i + 1] : 0),
                      (unsigned long)(i + 2 < n ? calls[i + 2] : 0),
                      (unsigned long)(i + 3 < n ? calls[i + 3] : 0));
    }
    lcd_update();
    crumb(CR_HANG_DRAWN, 1);

    /* Waiting for a button, as a panic leaves it */
    /* S5L frequency changes write the PMU through a blocking I2C mutex.
     * No owner can run once interrupts are off for this fatal screen. */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && CONFIG_CPU != S5L8702 && CONFIG_CPU != S5L8720
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif
#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif
    system_exception_wait();
    system_reboot();
    while (1);
}

static void hang_tick(void)
{
    struct core_entry *corep = __core_id_entry(CURRENT_CORE);
    uint32_t *ret = (uint32_t *)irq_stack_top - 1;
    long since = corep->next_tmo_check;

    if (hang_thread)
        return;
    if (holds)
    {
        stuck_since = 0;
        return;
    }
    if (TIME_BEFORE(since, hold_end))
        since = hold_end;
    if (ui_thread && ui_thread->state == STATE_BLOCKED
        && button_queue_count() > 0)
    {
        if (!stuck_since)
            stuck_since = current_tick;
        else if (TIME_AFTER(current_tick, stuck_since + STUCK_SECS * HZ))
            hang_stuck = true;
    }
    else
        stuck_since = 0;
    if (!hang_stuck &&
        !TIME_AFTER(current_tick, since + HANG_SECS * HZ))
        return;
    /* The IRQ handler's prologue pushes lr, already the interrupted pc,
     * highest of its registers, so at the top of the IRQ stack; its return
     * loads pc from there. The thread ran in system mode, whose sp and lr
     * are banked away from IRQ mode's. */
    hang_thread = corep->running;
    hang_pc = *ret;
    asm volatile ("mrs r2, cpsr      \n"
                  "msr cpsr_c, #0xdf \n"
                  "mov %0, sp        \n"
                  "mov %1, lr        \n"
                  "msr cpsr_c, r2    \n"
                  : "=r" (hang_sp), "=r" (hang_lr) : : "r2");
    *ret = (uint32_t)hang_show;
}

#ifdef HAVE_HANG_CATCH_FIQ
#include "hang_watch.h"

/* The freeze watch. The tick above needs IRQs; a timer FIQ does not, and
 * it also sees a UI thread that is runnable yet never switched in. What it
 * finds is drawn from the FIQ itself, on a stack of its own, as nothing
 * else is going to run again. */
void *hang_ui_thread;
long hang_ui_ran;
static bool fiq_armed;
static struct hang_watch fiq_watch;
static uint32_t fiq_stack[512] __attribute__((aligned(8)));
uint32_t *const hang_fiq_stack_end = &fiq_stack[512];

static struct
{
    int kind;
    uint32_t pc, cpsr, sp, lr;          /* where the FIQ came in */
    uint32_t isp, ilr, ispsr;           /* the IRQ bank: the last IRQ */
    const struct thread_entry *running;
    long tick, ui_ran;
} freeze;

/* A mode's banked sp and lr, read from the FIQ with both masks set. The
 * values travel in r0/r1 because r8-r12 are banked in FIQ mode. */
static void banked_regs(uint32_t mode, uint32_t *sp, uint32_t *lr)
{
    register uint32_t m asm("r0") = (mode == 0x10 ? 0x1f : mode) | 0xc0;
    register uint32_t s asm("r1");
    register uint32_t l asm("r2");
    asm volatile ("mrs   r3, cpsr     \n"
                  "msr   cpsr_c, r0   \n"
                  "mov   r1, sp       \n"
                  "mov   r2, lr       \n"
                  "msr   cpsr_c, r3   \n"
                  : "=r" (s), "=r" (l) : "r" (m) : "r3");
    *sp = s;
    *lr = l;
}

/* SYS mode has no SPSR, so only IRQ mode's is read */
static uint32_t irq_spsr(void)
{
    register uint32_t p asm("r1");
    asm volatile ("mrs   r3, cpsr     \n"
                  "msr   cpsr_c, #0xd2 \n"
                  "mrs   r1, spsr     \n"
                  "msr   cpsr_c, r3   \n"
                  : "=r" (p) : : "r3");
    return p;
}

int hang_catch_fiq(uint32_t pc, uint32_t spsr)
{
    struct core_entry *corep = __core_id_entry(CURRENT_CORE);
    const struct thread_entry *ui = hang_ui_thread;

    if (!fiq_armed || freeze.kind || hang_thread)
        return 0;
    bool waiting = ui && !holds && ui->state == STATE_RUNNING &&
                   corep->running != ui;
    int kind = hang_watch_step(&fiq_watch, current_tick, HZ, waiting,
                               hang_ui_ran);
    if (kind == HANG_WATCH_NONE)
        return 0;
    freeze.kind = kind;
    freeze.pc = pc;
    freeze.cpsr = spsr;
    freeze.running = corep->running;
    freeze.tick = current_tick;
    freeze.ui_ran = hang_ui_ran;
    uint32_t mode = spsr & 0x1f;
    if (mode == 0x11)           /* cannot happen: FIQs do not nest */
        freeze.sp = freeze.lr = 0;
    else
        banked_regs(mode, &freeze.sp, &freeze.lr);
    banked_regs(0x12, &freeze.isp, &freeze.ilr);
    freeze.ispsr = irq_spsr();
    return 1;
}

void hang_catch_fiq_show(void)
{
    crumb(CR_FREEZE, freeze.pc);
    crumb(CR_FREEZE_CONTEXT, (freeze.kind << 8) | (freeze.cpsr & 0xff));

    extern const char _irqstackend[];
    const struct thread_entry *t = freeze.running;
    uint32_t mode = freeze.cpsr & 0x1f;
    uint32_t top = (mode == 0x1f || mode == 0x10) && t
                 ? (uint32_t)t->stack + t->stack_size
                 : (uint32_t)_irqstackend;
    uint32_t calls[8];
    unsigned int y = 1;
    int n = stack_scan(freeze.sp, top, calls, 8);
    char name[8];

    panic_screen_enter();
    char message[80];
    name8(hang_ui_thread, name);
    if (freeze.kind == HANG_WATCH_IRQS_OFF)
        snprintf(message, sizeof(message), "IRQs off %ds: tick %ld stopped",
                 HANG_WATCH_IRQ_SECS, freeze.tick);
    else
        snprintf(message, sizeof(message), "%s starved %lds (runnable)", name,
                 (freeze.tick - freeze.ui_ran) / HZ);
#ifdef HAVE_CRUMBS
    crumbs_crash_record(message, &(struct crumb_crash_context) {
        .kind = CRUMB_CRASH_FREEZE, .pc = freeze.pc, .sp = freeze.sp,
        .lr = freeze.lr, .status = freeze.cpsr,
    });
#endif
    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, "*FREEZE* (" RBVERSION ")");
    lcd_puts(1, y++, message);
    name8(t, name);
    lcd_putsf(1, y++, "pc:%08lx cpsr:%02lx thr:%s",
              (unsigned long)freeze.pc, (unsigned long)(freeze.cpsr & 0xff),
              name);
    lcd_putsf(1, y++, "sp:%08lx lr:%08lx", (unsigned long)freeze.sp,
              (unsigned long)freeze.lr);
    lcd_putsf(1, y++, "irq sp:%08lx lr:%08lx spsr:%02lx",
              (unsigned long)freeze.isp, (unsigned long)freeze.ilr,
              (unsigned long)(freeze.ispsr & 0xff));
    lcd_update();               /* the first lines stay if the rest faults */
    crumb(CR_FREEZE_DRAWN, freeze.kind);
    for (int i = 0; i < n; i += 4)
        lcd_putsf(1, y++, "%08lx %08lx %08lx %08lx",
                  (unsigned long)calls[i],
                  (unsigned long)(i + 1 < n ? calls[i + 1] : 0),
                  (unsigned long)(i + 2 < n ? calls[i + 2] : 0),
                  (unsigned long)(i + 3 < n ? calls[i + 3] : 0));
    /* Each thread: state, where its switch saved lr, what it waits on (the
     * wait queue, so the object around it) and who called the wait */
    lcd_puts(1, y++, "thread  s pr lr       wq       caller");
    for (unsigned i = 0; i < MAXTHREADS; i++)
    {
        const struct thread_entry *e = __thread_slot_entry(i);
        if (e->state == STATE_KILLED)
            continue;
        name8(e, name);
#ifdef HAVE_PRIORITY_SCHEDULING
        int prio = e->priority;
        uint32_t ra = (uint32_t)e->blk_ra;
#else
        int prio = 0;
        uint32_t ra = 0;
#endif
        lcd_putsf(1, y++, "%-7.7s %c %2d %08lx %08lx %08lx", name,
                  e == t ? '*' : state_letter(e->state), prio,
                  (unsigned long)e->context.lr, (unsigned long)e->wqp,
                  (unsigned long)ra);
    }
    lcd_update();
    system_exception_wait();
    system_reboot();
    while (1);
}
#endif /* HAVE_HANG_CATCH_FIQ */

void hang_catch_init(void)
{
    int oldlevel = disable_irq_save();

    /* Outside an interrupt, IRQ mode's banked sp rests at its stack's top */
    asm volatile ("mrs r2, cpsr      \n"
                  "msr cpsr_c, #0xd2 \n"
                  "mov %0, sp        \n"
                  "msr cpsr_c, r2    \n"
                  : "=r" (irq_stack_top) : : "r2");
    restore_irq(oldlevel);
    ui_thread = __core_id_entry(CURRENT_CORE)->running;
    tick_add_task(hang_tick);
#ifdef HAVE_HANG_CATCH_FIQ
    hang_ui_ran = current_tick;
    hang_ui_thread = (void *)ui_thread;
    fiq_watch.tick = current_tick;
    fiq_watch.still = 0;
    fiq_armed = true;
    hang_watch_fiq_start();
#endif
}

void hang_catch_stop(void)
{
    crumbs_clean();             /* every intended power off and reboot */
#ifdef HAVE_HANG_CATCH_FIQ
    fiq_armed = false;
    hang_watch_fiq_stop();
#endif
    tick_remove_task(hang_tick);
}

void hang_catch_hold(bool hold)
{
    if (hold)
        holds++;
    else if (--holds == 0)
        hold_end = current_tick;
}
