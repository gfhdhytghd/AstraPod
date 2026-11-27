/* AstraPod animation scheduler checks on the real simulator kernel. GPLv2+. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "ui/motion_schedule.h"

static struct semaphore work, resumed;
static long worker_stack[DEFAULT_STACK_SIZE / sizeof(long)];

static void background(void)
{
    for (int i = 0; i < 2; i++)
    {
        semaphore_wait(&work, TIMEOUT_BLOCK);
        while (ui_motion_active())
            sleep(1);
        semaphore_release(&resumed);
    }
}

void sim_motion_probe(void)
{
    if (!getenv("PODBOX_TEST_MOTION"))
        return;

    assert(!ui_motion_active());
    semaphore_init(&work, 1, 0);
    semaphore_init(&resumed, 1, 0);
    unsigned worker = create_thread(background, worker_stack,
                                    sizeof(worker_stack), 0, "probe bg"
                                    IF_PRIO(, PRIORITY_BACKGROUND)
                                    IF_COP(, CPU));
    assert(worker);
#ifdef HAVE_PRIORITY_SCHEDULING
    const int normal = thread_get_priority(thread_self());
#endif
    struct ui_motion outer = ui_motion_begin("probe-outer");
    struct ui_motion inner = ui_motion_begin("probe-inner");
    assert(outer.lease && inner.lease && outer.lease != inner.lease);
    assert(!outer.trace && !inner.trace);
#ifdef HAVE_PRIORITY_SCHEDULING
    assert(thread_get_priority(thread_self()) < normal);
#endif
    semaphore_release(&work);
    ui_motion_end(&outer);
    assert(ui_motion_active());
    assert(semaphore_wait(&resumed, HZ / 10) == OBJ_WAIT_TIMEDOUT);
    ui_motion_end(&inner);
    assert(semaphore_wait(&resumed, HZ / 4) == OBJ_WAIT_SUCCEEDED);
    assert(!ui_motion_active());

    /* No call to ui_motion_active() before checking restoration. */
    struct ui_motion stale = ui_motion_begin("probe-timeout");
    sleep(2 * HZ + HZ / 4);
#ifdef HAVE_PRIORITY_SCHEDULING
    assert(thread_get_priority(thread_self()) == normal);
#endif
    assert(!ui_motion_active());

    struct ui_motion fresh = ui_motion_begin("probe-fresh");
    ui_motion_end(&stale);
    assert(ui_motion_active());
    semaphore_release(&work);
    assert(semaphore_wait(&resumed, HZ / 10) == OBJ_WAIT_TIMEDOUT);
    assert(semaphore_wait(&resumed, 3 * HZ) == OBJ_WAIT_SUCCEEDED);
    ui_motion_end(&fresh);
    assert(!ui_motion_active());
    thread_wait(worker);
    fprintf(stderr, "PASS motion: nesting, timeout, priority restoration and worker resume\n");
}
