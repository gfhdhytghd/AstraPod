/* AstraPod foreground motion scheduling. GPL version 2+. */
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "thread.h"
#include "motion_sched.h"
#include "ui/motion_schedule.h"
#include "ui/perf.h"

#ifdef IPOD_6G
/* Bound each scope independently, including interrupted gestures. */
#define MOTION_LIMIT (2 * HZ)
#define MOTION_SLOTS 8
#define PRIORITY_ANIMATION (PRIORITY_USER_INTERFACE - 2)

static struct {
    unsigned token;
    long until;
} scopes[MOTION_SLOTS];
static unsigned serial, ui_thread;
static unsigned scope_count;
static struct mutex lock;
static struct semaphore wake;
static bool ready;
#ifdef HAVE_PRIORITY_SCHEDULING
static int idle_priority;
#endif

static void drop_scope(unsigned slot)
{
    scopes[slot].token = 0;
    --scope_count;
#ifdef HAVE_PRIORITY_SCHEDULING
    if (!scope_count)
        thread_set_priority(ui_thread, idle_priority);
#endif
}

static void publish_scopes(void)
{
#ifdef HAVE_MOTION_SCHED
    long latest = current_tick;
    for (unsigned i = 0; i < MOTION_SLOTS; i++)
        if (scopes[i].token && TIME_AFTER(scopes[i].until, latest))
            latest = scopes[i].until;
    motion_sched_publish(scope_count != 0, latest);
#endif
}

static int expire_scopes(void)
{
    long now = current_tick;
    int delay = TIMEOUT_BLOCK;
    unsigned previous = scope_count;
    for (unsigned i = 0; i < MOTION_SLOTS; i++)
    {
        if (!scopes[i].token)
            continue;

        long left = (long)((unsigned long)scopes[i].until - now);
        if (left <= 0)
            drop_scope(i);
        else if (delay == TIMEOUT_BLOCK || left < delay)
            delay = left;
    }
    if (scope_count != previous)
        publish_scopes();
    return delay;
}

/* Restore the owner's base priority even when the owner is blocked. */
static void watchdog(void)
{
    while (true)
    {
        mutex_lock(&lock);
        int delay = expire_scopes();
        mutex_unlock(&lock);
        semaphore_wait(&wake, delay);
    }
}

void ui_motion_schedule_init(void)
{
    static long stack[DEFAULT_STACK_SIZE / sizeof(long)];
    ui_thread = thread_self();
    mutex_init(&lock);
    semaphore_init(&wake, 1, 0);
    ready = create_thread(watchdog, stack, sizeof(stack), 0, "ui motion"
                          IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU)) != 0;
}

static unsigned acquire_scope(unsigned previous)
{
    if (!ready || thread_self() != ui_thread)
        return 0;

    unsigned token = 0;
    mutex_lock(&lock);
    expire_scopes();
    if (previous)
    {
        for (unsigned i = 0; i < MOTION_SLOTS; i++)
        {
            if (scopes[i].token == previous)
            {
                scopes[i].until = (unsigned long)current_tick + MOTION_LIMIT;
                publish_scopes();
                mutex_unlock(&lock);
                return previous;
            }
        }
    }
    if (scope_count < MOTION_SLOTS)
    {
        unsigned slot = 0;
        while (scopes[slot].token)
            slot++;

        if (++serial == 0)
            serial++;
        scopes[slot].token = token = serial;
        scopes[slot].until = (unsigned long)current_tick + MOTION_LIMIT;
        if (scope_count++ == 0)
        {
#ifdef HAVE_PRIORITY_SCHEDULING
            int priority = thread_get_priority(ui_thread);
            idle_priority =
                thread_set_priority(ui_thread, MIN(priority, PRIORITY_ANIMATION));
#endif
            semaphore_release(&wake);
        }
        publish_scopes();
    }
    mutex_unlock(&lock);
    return token;
}

static void release_scope(unsigned token)
{
    if (!token || thread_self() != ui_thread)
        return;

    mutex_lock(&lock);
    for (unsigned i = 0; i < MOTION_SLOTS; i++)
    {
        if (token == scopes[i].token)
        {
            drop_scope(i);
            publish_scopes();
            break;
        }
    }
    mutex_unlock(&lock);
}

bool ui_motion_active(void)
{
    if (!ready)
        return false;

    mutex_lock(&lock);
    bool active = expire_scopes() != TIMEOUT_BLOCK;
    mutex_unlock(&lock);
    return active;
}
#else
void ui_motion_schedule_init(void) {}
bool ui_motion_active(void) { return false; }
#endif

struct ui_motion ui_motion_begin(const char *route)
{
    struct ui_motion motion = {0};
#ifdef IPOD_6G
    motion.lease = acquire_scope(0);
#endif
    motion.trace = ui_perf_begin(route);
    return motion;
}

void ui_motion_renew(struct ui_motion *motion)
{
#ifdef IPOD_6G
    motion->lease = acquire_scope(motion->lease);
#else
    (void)motion;
#endif
}

void ui_motion_end(struct ui_motion *motion)
{
#ifdef IPOD_6G
    release_scope(motion->lease);
#endif
    ui_perf_end(motion->trace);
    *motion = (struct ui_motion){0};
}
