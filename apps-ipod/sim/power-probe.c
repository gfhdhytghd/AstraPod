/* GPL version 2+. Counters for tools/tests/ui_power_sim.py.
 * The probe build runs the same UI and records boost requests without
 * changing the simulator's CPU frequency. */
#include <SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "backlight.h"
#include "input/action.h"
#include "lcd.h"

struct power_probe {
    uint64_t waits, polls, updates, pixels;
    uint64_t boosted_ns, boost_start_ns;
    uint64_t lcd_on, playing, context;
};

static struct power_probe *probe;
static uint64_t boost_depth;

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void probe_open(void)
{
    const char *path = getenv("PODBOX_POWER_COUNTERS");
    if (!path)
        return;
    FILE *file = fopen(path, "r+b");
    if (!file)
    {
        perror(path);
        abort();
    }
    probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE, MAP_SHARED,
                 fileno(file), 0);
    fclose(file);
    if (probe == MAP_FAILED)
        abort();
    if (boost_depth)
        probe->boost_start_ns = monotonic_ns();
}

void sim_power_boost(bool on)
{
    if (on)
    {
        if (boost_depth++ == 0 && probe)
            probe->boost_start_ns = monotonic_ns();
    }
    else if (boost_depth && --boost_depth == 0 && probe)
    {
        probe->boosted_ns += monotonic_ns() - probe->boost_start_ns;
        probe->boost_start_ns = 0;
    }
}

extern int audio_status(void);
extern int __real_get_action(int context, int timeout);
extern int __real_get_custom_action(int context, int timeout,
                                   const struct button_mapping *(*map)(int));

static void action_return(int context, int timeout)
{
    if (!probe)
        return;
    if (timeout)
        ++probe->waits;
    else
        ++probe->polls;
    probe->context = context;
    probe->lcd_on = lcd_active() && is_backlight_on(false);
    probe->playing = audio_status();
}

int __wrap_get_action(int context, int timeout)
{
    static bool opened;
    if (!opened)
    {
        opened = true;
        probe_open();
    }
    int result = __real_get_action(context, timeout);
    action_return(context, timeout);
    return result;
}

int __wrap_get_custom_action(int context, int timeout,
                            const struct button_mapping *(*map)(int))
{
    int result = __real_get_custom_action(context, timeout, map);
    action_return(context, timeout);
    return result;
}

extern SDL_Surface *lcd_surface;
extern void __real_sdl_gui_update(SDL_Surface *, int, int, int, int,
                                  int, int, int, int);

void __wrap_sdl_gui_update(SDL_Surface *surface, int x, int y, int width,
                           int height, int max_x, int max_y, int ox, int oy)
{
    if (probe && surface == lcd_surface)
    {
        ++probe->updates;
        probe->pixels += (uint64_t)SDL_min(width, max_x - x) *
                         SDL_min(height, max_y - y);
    }
    __real_sdl_gui_update(surface, x, y, width, height, max_x, max_y, ox, oy);
}
