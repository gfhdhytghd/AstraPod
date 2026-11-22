/* SDL worker backend for Wolf3D. GPL-2.0-or-later. */
#include "SDL_config.h"
#include "SDL_thread.h"
#include "../SDL_thread_c.h"
#include "../SDL_systhread.h"

#ifdef HAVE_SIGALTSTACK_THREADS
/* The hosted worker also carries the signal trampoline. */
#define WOLF_WORKER_STACK (80 * 1024)
#else
#define WOLF_WORKER_STACK (16 * 1024)
#endif

static long stack[WOLF_WORKER_STACK / sizeof(long)];
static unsigned int worker;

int SDL_SYS_CreateThread(SDL_Thread *thread, void *args)
{
    if (worker)
        return -1;
    worker = rb->rock_worker_start(SDL_RunThread, args, stack, sizeof(stack));
    thread->handle = worker;
    return worker ? 0 : -1;
}

void SDL_SYS_SetupThread(void)
{
}

Uint32 SDL_ThreadID(void)
{
    return rb->thread_self();
}

void SDL_SYS_WaitThread(SDL_Thread *thread)
{
    if (rb->rock_worker_join(thread->handle))
        worker = 0;
}

void SDL_SYS_KillThread(SDL_Thread *thread)
{
    /* The audio backend signals stop before joining its only worker. */
    (void)thread;
}
