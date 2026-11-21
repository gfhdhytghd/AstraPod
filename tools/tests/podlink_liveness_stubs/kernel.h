/* Rockbox kernel semantics on pthreads: owner-recursive mutexes that block,
 * counting semaphores with tick timeouts, tick sleep. */
#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include "system.h"
#include <pthread.h>
#define HZ 100
#define TIME_AFTER(a, b) ((long)((b) - (a)) < 0)
#define TIME_BEFORE(a, b) TIME_AFTER(b, a)
struct mutex
{
    pthread_mutex_t lock;
    pthread_cond_t open;
    pthread_t owner;
    bool held;
    int recursion;
};
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
struct semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count, maximum;
};
void semaphore_init(struct semaphore *s, int maximum, int count);
void semaphore_release(struct semaphore *s);
int semaphore_wait(struct semaphore *s, int ticks);
unsigned sleep(unsigned ticks);
#endif

#define TIMEOUT_BLOCK (-1)
