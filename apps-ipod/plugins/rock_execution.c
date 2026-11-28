/* Foreground execution leases. GNU GPL version 2 or later. */
#include <limits.h>
#include "kernel.h"
#include "timer.h"
#include "plugins/rock_execution.h"

#define ROCK_WORKERS 2
#define ROCK_MPEG_WORKERS 3

struct rock_worker {
    unsigned int id;
    void (*run)(void *);
    void *arg;
};

static struct rock_worker workers[ROCK_MPEG_WORKERS];
static unsigned int foreground;
static volatile bool stopping;
static void (*tick_callback)(void *);
static void *tick_arg;
static unsigned int ticks_queued;

void rock_execution_begin(void)
{
    stopping = false;
    foreground = thread_self();
}

static void worker_main(void)
{
    unsigned int id = thread_self();
    for (unsigned int i = 0; i < ROCK_MPEG_WORKERS; i++)
    {
        if (workers[i].id == id)
        {
            if (!rock_worker_stopping())
                workers[i].run(workers[i].arg);
            break;
        }
    }
    thread_exit();
}

static unsigned int start_worker(void (*run)(void *), void *arg,
                                 void *stack, size_t bytes, unsigned int limit)
{
    if (stopping || foreground != thread_self() || !run || !stack ||
        bytes < DEFAULT_STACK_SIZE || (uintptr_t)stack % sizeof(long) ||
        bytes % sizeof(long))
        return 0;

    for (unsigned int i = 0; i < limit; i++)
    {
        struct rock_worker *worker = &workers[i];
        if (worker->id)
            continue;

        unsigned int id = create_thread(worker_main, stack, bytes,
            CREATE_THREAD_FROZEN, "plugin worker"
            IF_PRIO(, PRIORITY_USER_INTERFACE) IF_COP(, CPU));
        if (id)
        {
            *worker = (struct rock_worker){ id, run, arg };
            thread_thaw(id);
        }
        return id;
    }
    return 0;
}

unsigned int rock_worker_start(void (*run)(void *), void *arg,
                               void *stack, size_t bytes)
{
    return start_worker(run, arg, stack, bytes, ROCK_WORKERS);
}

unsigned int rock_mpeg_worker_start(void (*run)(void *), void *arg,
                                    void *stack, size_t bytes)
{
    return start_worker(run, arg, stack, bytes, ROCK_MPEG_WORKERS);
}

bool rock_worker_stopping(void)
{
    /* Both firmware targets for this SDK schedule plugin workers on the
     * foreground core. Reload the flag across cooperative scheduling. */
    return stopping;
}

bool rock_worker_owned(unsigned int id)
{
    if (!id || stopping || foreground != thread_self())
        return false;
    for (unsigned int i = 0; i < ROCK_MPEG_WORKERS; i++)
        if (workers[i].id == id)
            return true;
    return false;
}

bool rock_worker_join(unsigned int id)
{
    if (!id || foreground != thread_self())
        return false;

    for (unsigned int i = 0; i < ROCK_MPEG_WORKERS; i++)
    {
        struct rock_worker *worker = &workers[i];
        if (worker->id != id)
            continue;
        thread_wait(id);
        *worker = (struct rock_worker){0};
        return true;
    }
    return false;
}

bool rock_timer_start(long cycles, void (*tick)(void *), void *arg)
{
    if (stopping || foreground != thread_self() || !tick || tick_callback ||
        !timer_reserve_ticks(cycles))
        return false;
    tick_callback = tick;
    tick_arg = arg;
    return true;
}

unsigned int rock_timer_dispatch(unsigned int limit)
{
    if (stopping || foreground != thread_self() || !tick_callback)
        return 0;

    unsigned int new_ticks = timer_take_ticks();
    ticks_queued += MIN(new_ticks, UINT_MAX - ticks_queued);
    unsigned int count = 0;
    while (count < limit && ticks_queued && tick_callback)
    {
        ticks_queued--;
        count++;
        tick_callback(tick_arg);
    }
    return count;
}

void rock_execution_stop_ticks(void)
{
    if (tick_callback)
        timer_release_ticks();
    tick_callback = NULL;
    tick_arg = NULL;
    ticks_queued = 0;
}

void rock_timer_stop(void)
{
    if (foreground == thread_self())
        rock_execution_stop_ticks();
}

void rock_execution_end(void)
{
    stopping = true;
    rock_execution_stop_ticks();
    for (unsigned int i = 0; i < ROCK_MPEG_WORKERS; i++)
        if (workers[i].id)
            rock_worker_join(workers[i].id);
    foreground = 0;
}
