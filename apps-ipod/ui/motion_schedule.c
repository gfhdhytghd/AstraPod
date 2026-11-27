/* AstraPod foreground motion scheduling. GPL version 2+. */
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "thread.h"
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
static unsigned serial, owner;
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
    if (--scope_count == 0)
    {
#ifdef HAVE_PRIORITY_SCHEDULING
        thread_set_priority(owner, idle_priority);
#endif
        owner = 0;
    }
}

static int expire_scopes(void)
{
    long now = current_tick;
    int delay = TIMEOUT_BLOCK;
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
    mutex_init(&lock);
    semaphore_init(&wake, 1, 0);
    ready = create_thread(watchdog, stack, sizeof(stack), 0, "ui motion"
                          IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU)) != 0;
}

static unsigned acquire_scope(unsigned previous)
{
    if (!ready)
        return 0;

    unsigned self = thread_self();
    unsigned token = 0;
    mutex_lock(&lock);
    expire_scopes();
    if (previous && owner == self)
    {
        for (unsigned i = 0; i < MOTION_SLOTS; i++)
        {
            if (scopes[i].token == previous)
            {
                scopes[i].until = (unsigned long)current_tick + MOTION_LIMIT;
                mutex_unlock(&lock);
                return previous;
            }
        }
    }
    if ((!owner || self == owner) && scope_count < MOTION_SLOTS)
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
            owner = self;
#ifdef HAVE_PRIORITY_SCHEDULING
            int priority = thread_get_priority(owner);
            idle_priority =
                thread_set_priority(owner, MIN(priority, PRIORITY_ANIMATION));
#endif
            semaphore_release(&wake);
        }
    }
    mutex_unlock(&lock);
    return token;
}

static void release_scope(unsigned token)
{
    if (!token)
        return;

    mutex_lock(&lock);
    if (thread_self() == owner)
    {
        for (unsigned i = 0; i < MOTION_SLOTS; i++)
        {
            if (token == scopes[i].token)
            {
                drop_scope(i);
                break;
            }
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
