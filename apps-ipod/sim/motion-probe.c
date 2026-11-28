/* AstraPod animation scheduler checks on the real simulator kernel. GPLv2+. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "audio/pcmbuf.h"
#include "database/tagcache.h"
#include "file.h"
#include "metadata/art_cache.h"
#include "playlist/playlist.h"
#include "rbpaths.h"
#include "system/activity.h"
#include "system/bg_task.h"
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

void sim_motion_work_started(const char *task)
{
    if (getenv("PODBOX_MOTION_LOAD_PROBE"))
        fprintf(stderr, "motion load: %s active at %ld\n", task, (long)current_tick);
}

static void probe_start_work(void)
{
    const char *database = ROCKBOX_DIR "/ui-motion-db.request";
    const char *art = ROCKBOX_DIR "/ui-motion-art.request";
    const char *play = ROCKBOX_DIR "/ui-motion-play.request";
    const char *finish = ROCKBOX_DIR "/ui-motion-finish.request";
    extern void bg_task_probe_wake(void);
    int previous = -1;

    for (;;)
    {
        sleep(0);
        int activity = get_current_activity();
        if (activity != previous)
        {
            previous = activity;
            if (activity == ACTIVITY_MAINMENU)
                fprintf(stderr, "motion probe: home\n");
            else if (activity == ACTIVITY_ALBUMCOVERS)
                fprintf(stderr, "motion probe: cover flow\n");
        }
        if (remove(finish) == 0)
        {
            struct pcmbuf_counters pcm = pcmbuf_get_counters();
            fprintf(stderr, "motion probe: PCM chunks=%u underruns=%u\n",
                    pcm.chunks_played, pcm.underruns);
        }
        if (remove(play) == 0)
        {
            int rc = playlist_create(NULL, NULL);
            if (rc >= 0)
                rc = playlist_insert_track(NULL, "/Play/00 Playback.flac",
                                           PLAYLIST_INSERT_LAST, false, true);
            assert(rc >= 0);
            playlist_start(0, 0, 0);
            fprintf(stderr, "motion probe: playback requested at %ld\n",
                    (long)current_tick);
        }
        if (remove(database) == 0)
        {
            tagcache_update();
            fprintf(stderr, "motion probe: database requested at %ld\n",
                    (long)current_tick);
        }
        if (remove(art) == 0)
        {
            bg_task_update(&art_cache_task);
            bg_task_probe_wake();
            fprintf(stderr, "motion probe: art requested at %ld\n",
                    (long)current_tick);
        }
    }
}

void sim_motion_probe(void)
{
    if (getenv("PODBOX_MOTION_LOAD_PROBE"))
    {
        static long load_stack[DEFAULT_STACK_SIZE / sizeof(long)];
        unsigned load = create_thread(probe_start_work, load_stack,
                                      sizeof(load_stack), 0, "motion load"
                                      IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));
        assert(load);
    }

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
