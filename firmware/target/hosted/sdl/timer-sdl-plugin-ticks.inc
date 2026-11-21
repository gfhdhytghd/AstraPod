/* Foreground plugin support. GNU GPL version 2 or later. */

#ifdef ASTRAPOD_ROCK_COMPAT
#include <SDL_atomic.h>
#include <stdint.h>

static SDL_SpinLock ticks_lock;
static SDL_TimerID ticks_id;
static uintptr_t ticks_generation;
static Uint32 ticks_time;
static unsigned int pending_ticks;
#endif

#ifdef ASTRAPOD_ROCK_COMPAT
static Uint32 count_tick(Uint32 interval, void *generation)
{
    SDL_AtomicLock(&ticks_lock);
    bool active = ticks_id && (uintptr_t)generation == ticks_generation;
    if (active)
    {
        /* The host can wake late. Keep the tick count on the SDL clock. */
        Uint32 elapsed = SDL_GetTicks() - ticks_time;
        unsigned int ticks = elapsed / interval;
        ticks_time += ticks * interval;
        pending_ticks += ticks;
    }
    SDL_AtomicUnlock(&ticks_lock);
    return active ? interval : 0;
}

bool timer_reserve_ticks(long cycles)
{
    /* Round up to SDL's millisecond resolution. */
    if (ticks_id || timer_prio != -1 || cycles <= 0)
        return false;

    const unsigned long cycles_per_ms = TIMER_FREQ / 1000;
    Uint32 interval = ((unsigned long)cycles - 1) / cycles_per_ms + 1;
    SDL_AtomicLock(&ticks_lock);
    ticks_time = SDL_GetTicks();
    ticks_id = SDL_AddTimer(interval, count_tick, (void *)++ticks_generation);
    SDL_AtomicUnlock(&ticks_lock);
    return ticks_id != 0;
}

unsigned int timer_take_ticks(void)
{
    SDL_AtomicLock(&ticks_lock);
    unsigned int ticks = pending_ticks;
    pending_ticks = 0;
    SDL_AtomicUnlock(&ticks_lock);
    return ticks;
}

void timer_release_ticks(void)
{
    SDL_AtomicLock(&ticks_lock);
    SDL_TimerID id = ticks_id;
    ticks_id = 0;
    pending_ticks = 0;
    SDL_AtomicUnlock(&ticks_lock);
    /* A late callback from this timer sees an inactive or newer generation. */
    if (id)
        SDL_RemoveTimer(id);
}
#endif
