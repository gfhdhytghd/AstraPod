/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The freeze watch's decision, apart from any hardware so the host tests
 * run this very code (tools/tests/hang_watch_test.c). A timer that does not
 * depend on IRQs (the S5L8702's timer G as an FIQ) calls hang_watch_step()
 * HANG_WATCH_HZ times a second.
 *
 * IRQs off: the kernel tick has not moved for HANG_WATCH_IRQ_SECS. The tick
 * is a 100 Hz IRQ; a section that masks IRQs for even 10 ms already drops
 * ticks and USB service, and the long masked stretches that are meant
 * (panic and HANG screens, ROLO, power off, reboot) also mask the FIQ, so
 * they cannot be counted. Deep sleep stops both timers' clock together.
 *
 * UI starved: the UI thread is runnable but has not been switched in for
 * HANG_WATCH_UI_SECS. Threads are cooperative, so this is another thread
 * holding the core. A UI thread that is itself busy is the running thread
 * and never counts; nor does one blocked on keys or a queue, as it is not
 * runnable. The hang catcher's own holds (skin loading) are excluded by the
 * caller. Five seconds is half the older core-held check, which stays.
 ****************************************************************************/
#ifndef HANG_WATCH_H
#define HANG_WATCH_H

#include <stdbool.h>

#define HANG_WATCH_HZ        10
#define HANG_WATCH_IRQ_SECS  3
#define HANG_WATCH_UI_SECS   5

enum hang_watch_kind
{
    HANG_WATCH_NONE = 0,
    HANG_WATCH_IRQS_OFF,
    HANG_WATCH_UI_STARVED,
};

struct hang_watch
{
    long tick;      /* the tick last seen */
    int still;      /* periods it has not moved */
};

/* ui_waiting: the UI thread is runnable, is not the running thread and no
 * hold is open. ui_ran: the tick it was last switched in. */
static inline enum hang_watch_kind hang_watch_step(struct hang_watch *w,
    long now, int hz, bool ui_waiting, long ui_ran)
{
    if (now != w->tick)
    {
        w->tick = now;
        w->still = 0;
    }
    else if (w->still < HANG_WATCH_IRQ_SECS * HANG_WATCH_HZ)
        w->still++;
    if (w->still >= HANG_WATCH_IRQ_SECS * HANG_WATCH_HZ)
        return HANG_WATCH_IRQS_OFF;
    if (ui_waiting && (long)((unsigned long)now - (unsigned long)ui_ran) >
                      (long)HANG_WATCH_UI_SECS * hz)
        return HANG_WATCH_UI_STARVED;
    return HANG_WATCH_NONE;
}

#endif /* HANG_WATCH_H */
