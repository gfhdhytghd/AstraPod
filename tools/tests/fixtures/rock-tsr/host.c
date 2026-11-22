/* Deterministic stop and drain probe for the production TSR runtime. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "plugins/sdk/plugin.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static _Thread_local unsigned id = 1;
static unsigned closed, stopped, admitted;
static bool fail_start, tick_ready, tick_held, stop_sent, joined;
static enum rock_tsr_event reason;
static struct plugin_header header;
static void (*entry)(void);

unsigned thread_self(void)
{
    return id;
}

static void *start_worker(void *unused)
{
    (void)unused;
    id = 2;
    entry();
    assert(0);
    return NULL;
}

unsigned create_thread(void (*start)(void), void *stack, unsigned bytes,
                       unsigned flags, const char *name)
{
    assert(stack && bytes >= DEFAULT_STACK_SIZE && !flags && name);
    if (fail_start)
        return 0;
    entry = start;
    assert(!pthread_create(&worker, NULL, start_worker, NULL));
    return 2;
}

void thread_wait(unsigned target)
{
    assert(target == 2);
    assert(!pthread_join(worker, NULL));
    joined = true;
}

void thread_exit(void)
{
    pthread_exit(NULL);
}

void semaphore_init(struct semaphore *sem, int max, int initial)
{
    assert(max == 1 && initial == 0);
    sem->count = initial;
    stop_sent = joined = false;
}

int semaphore_wait(struct semaphore *sem, int period)
{
    assert(period == 1);
    pthread_mutex_lock(&lock);
    while (!sem->count && !tick_ready)
        pthread_cond_wait(&changed, &lock);
    int result = sem->count ? 0 : OBJ_WAIT_TIMEDOUT;
    tick_ready = false;
    pthread_mutex_unlock(&lock);
    return result;
}

void semaphore_release(struct semaphore *sem)
{
    pthread_mutex_lock(&lock);
    sem->count = 1;
    stop_sent = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

void lc_close(void *handle)
{
    assert(handle == &header);
    if (stop_sent)
        assert(joined);
    closed++;
}

static void callback(enum rock_tsr_event event)
{
    if (event == ROCK_TSR_TICK)
    {
        assert(thread_self() == 2);
        pthread_mutex_lock(&lock);
        admitted++;
        pthread_cond_broadcast(&changed);
        while (tick_held)
            pthread_cond_wait(&changed, &lock);
        pthread_mutex_unlock(&lock);
        return;
    }

    if (stop_sent)
        assert(thread_self() == 2 && !tick_held);
    reason = event;
    stopped++;
}

static void *allow_drain(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stop_sent)
        pthread_cond_wait(&changed, &lock);
    assert(closed == 4 && stopped == 3);
    tick_held = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    assert(!rock_tsr_register(callback, 1));
    rock_tsr_begin(&header, &header);
    id = 99;
    assert(!rock_tsr_register(callback, 1));
    id = 1;
    assert(!rock_tsr_register(NULL, 1));
    assert(!rock_tsr_register(callback, 0));
    assert(!rock_tsr_register((rock_tsr_callback)(uintptr_t)1, 1));
    assert(rock_tsr_register(callback, 1));
    assert(!rock_tsr_register(callback, 1));
    assert(rock_tsr_finish(false));
    assert(closed == 1 && stopped == 1 && reason == ROCK_TSR_ABORT);

    rock_tsr_begin(&header, &header);
    assert(rock_tsr_finish(true));
    assert(closed == 2 && stopped == 1);

    rock_tsr_begin(&header, &header);
    assert(rock_tsr_register(callback, 1));
    fail_start = true;
    assert(!rock_tsr_finish(true));
    assert(closed == 3 && stopped == 2 && reason == ROCK_TSR_ABORT);
    fail_start = false;

    rock_tsr_begin(&header, &header);
    assert(rock_tsr_register(callback, 1));
    rock_tsr_stop(ROCK_TSR_USB);
    assert(closed == 3 && stopped == 2);
    assert(rock_tsr_finish(true));
    assert(closed == 4 && stopped == 3 && reason == ROCK_TSR_USB);

    rock_tsr_begin(&header, &header);
    assert(rock_tsr_register(callback, 1));
    assert(rock_tsr_finish(true));
    assert(!rock_tsr_register(callback, 1));
    pthread_mutex_lock(&lock);
    tick_ready = tick_held = true;
    pthread_cond_broadcast(&changed);
    while (!admitted)
        pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    pthread_t drain;
    assert(!pthread_create(&drain, NULL, allow_drain, NULL));
    rock_tsr_stop(ROCK_TSR_USB);
    assert(!pthread_join(drain, NULL));
    assert(joined && closed == 5 && stopped == 4 && admitted == 1);
    assert(reason == ROCK_TSR_USB);

    rock_tsr_begin(&header, &header);
    assert(rock_tsr_register(callback, 1));
    assert(rock_tsr_finish(true));
    rock_tsr_stop(ROCK_TSR_POWER);
    rock_tsr_stop(ROCK_TSR_REPLACED);
    assert(joined && closed == 6 && stopped == 5 && admitted == 1);
    assert(reason == ROCK_TSR_POWER);
    puts("PASS TSR registration, abort, thread failure, synchronous USB/power drain");
    return 0;
}
