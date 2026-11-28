/* AstraPod animation scheduler checks on the real simulator kernel. GPLv2+. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "motion_sched.h"
#include "ui/motion_schedule.h"

static struct semaphore work, resumed;
static long worker_stack[DEFAULT_STACK_SIZE / sizeof(long)];

static void probe_background(void)
{
    for (int i = 0; i < 2; i++)
    {
        semaphore_wait(&work, TIMEOUT_BLOCK);
        while (ui_motion_active())
            sleep(1);
        semaphore_release(&resumed);
    }
}

#ifdef HAVE_MOTION_SCHED
static long audio_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static struct mutex test_disk;
static unsigned dispatch_count, normal_order, audio_order;

static void normal_request(void)
{
    mutex_lock(&test_disk);
    normal_order = ++dispatch_count;
    mutex_unlock(&test_disk);
}

static void audio_request(void)
{
    mutex_lock(&test_disk);
    audio_order = ++dispatch_count;
    mutex_unlock(&test_disk);
}

static void probe_bands(void)
{
    mutex_init(&test_disk);
    motion_sched_set_disk_mutex(&test_disk);
    mutex_lock(&test_disk);

    struct ui_motion motion = ui_motion_begin("probe-bands");
    assert(motion_sched_active());
    unsigned normal = create_thread(normal_request, worker_stack,
        sizeof(worker_stack), 0, "probe normal", PRIORITY_BACKGROUND);
    assert(normal);
    for (int i = 0; i < 72; i++)
        yield();
    /* Sleeping lets normal enqueue first, behind the UI-owned object. */
    assert(test_disk.blocker.priority == PRIORITY_IDLE);
    sleep(2);
    assert(test_disk.blocker.priority == PRIORITY_BACKGROUND);

    unsigned audio = create_thread(audio_request, audio_stack,
        sizeof(audio_stack), 0, "probe audio", PRIORITY_MOTION_AUDIO);
    assert(audio);
    yield();
    /* PIP put the UI owner in the audio band. */
    assert(test_disk.blocker.priority == PRIORITY_MOTION_AUDIO);
    mutex_unlock(&test_disk);
    assert(audio_order == 1 && normal_order == 0);
    ui_motion_end(&motion);
    thread_wait(audio);
    thread_wait(normal);
    assert(normal_order == 2);
    motion_sched_set_disk_mutex(NULL);
    fprintf(stderr, "PASS bands: normal excluded, priority inherited, audio first\n");
}
#endif

void sim_motion_probe(void)
{
    if (!getenv("PODBOX_TEST_MOTION"))
        return;

    assert(!ui_motion_active());
    semaphore_init(&work, 1, 0);
    semaphore_init(&resumed, 1, 0);
    unsigned worker = create_thread(probe_background, worker_stack,
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
#ifdef HAVE_MOTION_SCHED
    probe_bands();
#endif
    fprintf(stderr, "PASS motion: nesting, timeout, priority restoration and worker resume\n");
}
