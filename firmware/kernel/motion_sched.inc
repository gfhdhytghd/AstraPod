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

/* Optional wait attribution for a single capture owner. */
static unsigned watch_id;
static uint32_t watch_start;
static bool watch_pending;
static unsigned watch_kind;
static struct motion_sched_wait watch_wait;

void motion_sched_watch(unsigned thread_id)
{
    watch_id = thread_id;
    watch_pending = false;
    watch_wait = (struct motion_sched_wait){0};
}

struct motion_sched_wait motion_sched_get_wait(void)
{
    return watch_wait;
}

static void motion_watch_out(const struct thread_entry *thread)
{
    if (!watch_id || !thread || thread->id != watch_id)
        return;
    watch_start = MOTION_TIMER;
    watch_pending = true;
    watch_kind = thread->state == STATE_RUNNING ? 0 : thread->blocker ? 1 : 2;
    if (watch_kind == 1)
        watch_wait.lock_site = (uintptr_t)thread->blk_ra;
}

static void motion_watch_in(const struct thread_entry *thread)
{
    if (!watch_pending || thread->id != watch_id)
        return;
    uint32_t elapsed = MOTION_TIMER - watch_start;
    if (watch_kind == 0)
        watch_wait.runnable += elapsed;
    else if (watch_kind == 1)
        watch_wait.inherited += elapsed;
    else
        watch_wait.other += elapsed;
    watch_pending = false;
}

#endif
