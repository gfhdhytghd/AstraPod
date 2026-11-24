/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "thread.h"
#include "timer.h"

static unsigned int frame_wait_thread;
static uint32_t frame_deadline;
static volatile bool frame_wake_pending;

void button_set_frame_deadline(const uint32_t *deadline)
{
    if (deadline)
        frame_deadline = *deadline;
    frame_wait_thread = deadline ? thread_self() : 0;
}

static void button_frame_ready(void)
{
    if ((int32_t)(frame_deadline - (uint32_t)USEC_TIMER) > 0)
        return;

    timer_stop();
    /* An input event already in the queue wakes the same waiter. */
    if (queue_empty(&button_queue))
    {
        frame_wake_pending = true;
        queue_post(&button_queue, SYS_TIMEOUT, 0);
    }
}

/* Called with IRQs disabled, so real input can replace an unconsumed wake. */
static void button_discard_frame_wake(void)
{
    if (frame_wake_pending)
    {
        queue_remove_from_head(&button_queue, SYS_TIMEOUT);
        frame_wake_pending = false;
    }
}

static void button_frame_wait(struct queue_event *ev, int ticks)
{
    bool armed = false;
    if (ticks > 0 && frame_wait_thread && frame_wait_thread == thread_self())
    {
        int32_t left = frame_deadline - (uint32_t)USEC_TIMER;
        if (left <= 0)
            ticks = TIMEOUT_NOBLOCK;
        else if (((uint32_t)left - 1) / (1000000u / HZ) < (uint32_t)ticks)
        {
            int irq = disable_irq_save();
            if (queue_empty(&button_queue))
                armed = timer_register(0, NULL, left * (TIMER_FREQ / 1000000),
                                       button_frame_ready);
            restore_irq(irq);
        }
    }

    /* Leave one tick of slack for the fallback's tick rounding. */
    queue_wait_w_tmo(&button_queue, ev, ticks + armed);
    if (armed)
    {
        int irq = disable_irq_save();
        if (pfn_timer == button_frame_ready)
            timer_unregister();
        button_discard_frame_wake();
        restore_irq(irq);
    }
}
