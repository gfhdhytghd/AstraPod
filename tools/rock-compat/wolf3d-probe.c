/* Synthetic SDL lifecycle probe. GPL-2.0-or-later. */
#include "SDL.h"
#include "SDL_mixer.h"

static volatile bool mixed;

static void tone(void *context, Uint8 *output, int bytes)
{
    (void)context;
    Sint16 *samples = (Sint16 *)output;
    for (int i = 0; i < bytes / (int)sizeof(*samples); ++i)
        samples[i] = (i & 32) ? 4096 : -4096;
    mixed = true;
}

static void mark(const char *name)
{
    int fd = rb->open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || rb->close(fd) < 0)
        wolf_error("Cannot record the Wolf3D probe result");
}

int __wrap_my_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
        wolf_error("Could not initialize probe SDL");
    SDL_Surface *screen = SDL_SetVideoMode(320, 200, LCD_DEPTH, SDL_SWSURFACE);
    if (!screen)
        wolf_error("Could not set probe SDL video mode");
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 8, 80, 180));
    SDL_Flip(screen);

    if (Mix_OpenAudio(wolf_audio_rate(), AUDIO_S16SYS, 2, 512) < 0)
        wolf_error("Could not start probe PCM");
    Mix_SetPostMix(tone, NULL);
    Uint32 deadline = SDL_GetTicks() + 2000;
    while ((!mixed || rb->rock_stream_pending() <= 0) &&
           SDL_GetTicks() < deadline)
        SDL_Delay(5);
    if (!mixed || rb->rock_stream_pending() <= 0)
        wolf_error("Probe PCM did not reach the foreground stream");
    mark("/.rockbox/wolf3d-probe-ready");

    SDL_Event event;
    while (SDL_WaitEvent(&event))
    {
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RETURN)
            mark("/.rockbox/wolf3d-probe-tap");
        if (event.type == SDL_KEYUP && event.key.keysym.sym == SDLK_RETURN)
            mark("/.rockbox/wolf3d-probe-release");
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
        {
            mark("/.rockbox/wolf3d-probe-return");
            return 0;
        }
    }
    return 1;
}
