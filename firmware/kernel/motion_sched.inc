/* iPod Classic motion policy, included by thread.c. GPL version 2+. */
#ifdef HAVE_MOTION_SCHED

#ifndef USEC_TIMER
#include <SDL_timer.h>
static uint32_t motion_timer(void)
{
    uint64_t counter = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    return (uint32_t)(counter / frequency * 1000000u +
                      counter % frequency * 1000000u / frequency);
}
#define MOTION_TIMER motion_timer()
#else
#define MOTION_TIMER ((uint32_t)USEC_TIMER)
#endif

static bool motion_active;
static long motion_deadline;
static struct blocker *disk_blocker;

void motion_sched_publish(bool active, long deadline)
{
    int oldlevel = disable_irq_save();
    motion_deadline = deadline;
    motion_active = active;
    restore_irq(oldlevel);
}

bool motion_sched_active(void)
{
    return motion_active && TIME_BEFORE(current_tick, motion_deadline);
}

bool motion_sched_background_io(void)
{
    struct thread_entry *self = __running_self_entry();
    return self->base_priority >= PRIORITY_BACKGROUND;
}

bool motion_sched_defer_io(void)
{
    struct thread_entry *self = __running_self_entry();
    /* Keep dependencies of foreground and audio eligible. */
    return motion_sched_active() && motion_sched_background_io() &&
           self->priority > PRIORITY_MOTION_UI;
}

void motion_sched_set_disk_mutex(struct mutex *mutex)
{
    disk_blocker = mutex ? &mutex->blocker : NULL;
}

static struct thread_entry *motion_disk_waiter(struct thread_entry *first)
{
    if (!disk_blocker || first->blocker != disk_blocker)
        return first;

    struct thread_entry *best = first;
    for (struct thread_entry *t = WQ_THREAD_NEXT(first); t; t = WQ_THREAD_NEXT(t))
    {
        if (t->priority < best->priority)
            best = t;
    }
    return best;
}

/* Classify the effective priority so PIP owners run in the waiter's band. */
static int motion_group_end(int highest)
{
    if (!motion_sched_active())
        return LOWEST_PRIORITY;
    if (highest <= PRIORITY_REALTIME)
        return PRIORITY_REALTIME;
    if (highest <= PRIORITY_MOTION_AUDIO)
        return PRIORITY_MOTION_AUDIO;
    if (highest <= PRIORITY_MOTION_UI)
        return PRIORITY_MOTION_UI;
    return LOWEST_PRIORITY;
}

#if defined(SIMULATOR) && defined(__linux__)
#include <time.h>
static uint32_t motion_cpu_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (uint32_t)now.tv_sec * 1000000u + now.tv_nsec / 1000u;
}
#else
#define motion_cpu_time() 0u
#endif

/* Optional wait attribution for a single capture owner. */
static unsigned watch_id;
static bool watch_pending, watch_woken, watch_inherited;
static unsigned watch_state;
static long watch_tick, watch_deadline, watch_wake_tick;
static uintptr_t watch_site;
static uint32_t watch_start, watch_wake, runner_start, runner_us;
static uint32_t runner_cpu_start, runner_cpu_us;
static const char *runner_current, *runner_name;
static struct motion_sched_wait watch_wait;

void motion_sched_watch(unsigned thread_id)
{
    watch_id = thread_id;
    watch_pending = false;
    watch_wait = (struct motion_sched_wait){0};
}

struct motion_sched_wait motion_sched_take_wait(void)
{
    struct motion_sched_wait wait = watch_wait;
    watch_wait = (struct motion_sched_wait){0};
    return wait;
}

static void motion_watch_wake(const struct thread_entry *thread)
{
    if (watch_pending && !watch_woken && thread->id == watch_id)
    {
        watch_wake = MOTION_TIMER;
        watch_wake_tick = current_tick;
        watch_woken = true;
    }
}

static void motion_watch_out(const struct thread_entry *thread)
{
    if (!watch_id || !thread || thread->id != watch_id)
        return;

    watch_start = MOTION_TIMER;
    runner_start = watch_start;
    runner_us = runner_cpu_us = 0;
    runner_cpu_start = motion_cpu_time();
    runner_name = NULL;
    runner_current = "scheduler";
    watch_tick = current_tick;
    watch_state = thread->state;
    watch_deadline = watch_state >= TIMEOUT_STATE_FIRST ? thread->tmo_tick : watch_tick;
    watch_site = watch_state == STATE_SLEEPING || watch_state == STATE_RUNNING
        ? 0 : (uintptr_t)thread->blk_ra;
    watch_pending = true;
    watch_woken = false;
    watch_inherited = thread->blocker != NULL;
    if (watch_inherited)
        watch_wait.lock_site = watch_site;
}

static void motion_watch_in(const struct thread_entry *thread)
{
    if (!watch_pending)
        return;
    uint32_t now = MOTION_TIMER;
    uint32_t cpu = motion_cpu_time();
    uint32_t slice = now - runner_start;
    if (slice > runner_us)
    {
        runner_us = slice;
        runner_name = runner_current;
        runner_cpu_us = cpu - runner_cpu_start;
    }
    runner_start = now;
    runner_cpu_start = cpu;
    runner_current = thread ? thread->name : "idle";
    if (!thread || thread->id != watch_id)
        return;

    uint32_t elapsed = now - watch_start;
    if (watch_state == STATE_RUNNING)
        watch_wait.runnable += elapsed;
    else if (watch_inherited)
        watch_wait.inherited += elapsed;
    else
    {
        watch_wait.other += elapsed;
        if (watch_state == STATE_SLEEPING)
            watch_wait.sleep_us += elapsed;
        else
            watch_wait.queue_us += elapsed;
    }
    if (elapsed > watch_wait.longest_us)
    {
        watch_wait.longest_us = elapsed;
        watch_wait.wait_site = watch_site;
        watch_wait.wait_ticks = watch_state >= TIMEOUT_STATE_FIRST
            ? watch_deadline - watch_tick : -1;
        watch_wait.wake_late_ticks = watch_woken ? watch_wake_tick - watch_deadline : 0;
        watch_wait.ready_us = watch_woken ? now - watch_wake : 0;
        watch_wait.runner_us = runner_us;
        watch_wait.runner_cpu_us = runner_cpu_us;
        unsigned i = 0;
        if (runner_name)
            for (; i + 1 < sizeof(watch_wait.runner) && runner_name[i]; i++)
                watch_wait.runner[i] = runner_name[i];
        watch_wait.runner[i] = '\0';
    }
    watch_pending = false;
}

#endif
