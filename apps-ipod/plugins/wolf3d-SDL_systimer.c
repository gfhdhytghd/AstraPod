/* SDL tick compatibility for Wolf3D. GPL-2.0-or-later. */
#include "SDL_config.h"
#include "SDL_timer.h"
#include "../SDL_timer_c.h"

static unsigned long origin;

void SDL_StartTicks(void)
{
    origin = *rb->current_tick;
}

Uint32 SDL_GetTicks(void)
{
    return (Uint32)(*rb->current_tick - origin) * (1000 / HZ);
}

void SDL_Delay(Uint32 ms)
{
    unsigned long deadline = *rb->current_tick +
        ms / (1000 / HZ) + (ms % (1000 / HZ) != 0);
    do
    {
        wolf_service();
        if (!TIME_BEFORE(*rb->current_tick, deadline))
            break;
        rb->sleep(1);
    }
    while (true);
}

int SDL_SYS_TimerInit(void)
{
    SDL_SetError("Wolf3D does not install SDL timer callbacks");
    return -1;
}

void SDL_SYS_TimerQuit(void)
{
}

int SDL_SYS_StartTimer(void)
{
    return -1;
}

void SDL_SYS_StopTimer(void)
{
}
