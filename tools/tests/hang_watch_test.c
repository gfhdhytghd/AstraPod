/* FU50: the freeze watch's decision (firmware/kernel/include/hang_watch.h),
 * compiled unchanged for the host. Each step is one 100 ms timer FIQ; the
 * kernel tick is 100 Hz. No device access. GPL version 2 or later. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "hang_watch.h"

#define HZ 100
#define PERIOD (HZ / HANG_WATCH_HZ)

static int steps_until(struct hang_watch *w, long *now, bool tick_runs,
                       bool ui_waiting, long ui_ran, int most,
                       enum hang_watch_kind *kind)
{
    for (int i = 1; i <= most; i++)
    {
        if (tick_runs)
            *now = (long)((unsigned long)*now + PERIOD);
        *kind = hang_watch_step(w, *now, HZ, ui_waiting, ui_ran);
        if (*kind != HANG_WATCH_NONE)
            return i;
    }
    return 0;
}

int main(void)
{
    enum hang_watch_kind kind;
    struct hang_watch w = { 0, 0 };
    long now = 0;

    /* A running tick never trips it, for a simulated day */
    assert(!steps_until(&w, &now, true, false, 0, 24 * 3600 * HANG_WATCH_HZ,
                        &kind));

    /* IRQs off: one period short of 3 s is still fine; 3 s trips */
    w = (struct hang_watch){ now, 0 };
    assert(!steps_until(&w, &now, false, false, now,
                        HANG_WATCH_IRQ_SECS * HANG_WATCH_HZ - 1, &kind));
    assert(steps_until(&w, &now, false, false, now, 1, &kind) == 1);
    assert(kind == HANG_WATCH_IRQS_OFF);
    /* and it stays tripped (the count saturates, never wraps) */
    for (int i = 0; i < 100000; i++)
        assert(hang_watch_step(&w, now, HZ, false, now) == HANG_WATCH_IRQS_OFF);

    /* A stall that ends before 3 s resets the count */
    w = (struct hang_watch){ now, 0 };
    assert(!steps_until(&w, &now, false, false, now, 25, &kind));
    now = (long)((unsigned long)now + 1);
    assert(hang_watch_step(&w, now, HZ, false, now) == HANG_WATCH_NONE);
    assert(!steps_until(&w, &now, false, false, now, 25, &kind));

    /* UI runnable but never switched in: 5 s exactly is fine, past it trips */
    w = (struct hang_watch){ now, 0 };
    long ran = now;
    int n = steps_until(&w, &now, true, true, ran, 1000, &kind);
    assert(kind == HANG_WATCH_UI_STARVED);
    assert(n == HANG_WATCH_UI_SECS * HANG_WATCH_HZ + 1);

    /* A UI thread that is blocked (keys, queue) or itself running is not
     * waiting: hours pass without a trip */
    w = (struct hang_watch){ now, 0 };
    ran = now;
    assert(!steps_until(&w, &now, true, false, ran, 3600 * HANG_WATCH_HZ,
                        &kind));

    /* A UI thread switched in every second never trips */
    w = (struct hang_watch){ now, 0 };
    for (int s = 0; s < 3600; s++)
    {
        ran = now;
        assert(!steps_until(&w, &now, true, true, ran, HANG_WATCH_HZ, &kind));
    }

    /* Across the tick's wrap */
    now = LONG_MAX - 2 * HZ;
    w = (struct hang_watch){ now, 0 };
    ran = now;
    n = steps_until(&w, &now, true, true, ran, 1000, &kind);
    assert(kind == HANG_WATCH_UI_STARVED);
    assert(n == HANG_WATCH_UI_SECS * HANG_WATCH_HZ + 1);
    now = LONG_MAX - 5;
    w = (struct hang_watch){ now, 0 };
    assert(!steps_until(&w, &now, true, false, now, 100, &kind));

    puts("hang_watch_test: ok");
    return 0;
}
