/* SDL 1.2 subset used by Quake's indexed software display. GPL-2.0-or-later. */
#ifndef ASTRAPOD_QUAKE_SDL_H
#define ASTRAPOD_QUAKE_SDL_H
#include "quake_libc.h"
#include "include/SDL_keysym.h"

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef struct { Uint8 r, g, b; } SDL_Color;
typedef struct { int w, h, pitch; void *pixels; } SDL_Surface;
typedef struct {
    int type;
    struct { struct { SDLKey sym; } keysym; int state; } key;
    struct { int x, y, xrel, yrel; } motion;
} SDL_Event;

enum { SDL_KEYDOWN, SDL_KEYUP, SDL_MOUSEMOTION, SDL_QUIT };
enum { SDL_INIT_VIDEO = 1, SDL_INIT_AUDIO = 2 };
enum { SDL_SWSURFACE = 0, SDL_HWPALETTE = 1, SDL_FULLSCREEN = 2 };
#define SDL_LIL_ENDIAN 1234
#define SDL_BYTEORDER SDL_LIL_ENDIAN

int quake_sdl_init(unsigned int flags);
void quake_sdl_quit(void);
SDL_Surface *quake_sdl_mode(int w, int h, int bits, unsigned int flags);
void quake_sdl_palette(SDL_Surface *surface, SDL_Color *colors, int first,
                       int count);
void quake_sdl_update(SDL_Surface *surface, int x, int y, int w, int h);
int quake_sdl_poll(SDL_Event *event);

#define SDL_Init quake_sdl_init
#define SDL_Quit quake_sdl_quit
#define SDL_SetVideoMode quake_sdl_mode
#define SDL_SetColors quake_sdl_palette
#define SDL_UpdateRect quake_sdl_update
#define SDL_PollEvent quake_sdl_poll
#define SDL_GetError() "Quake display unavailable"
#define SDL_GetModState() KMOD_NONE
#define SDL_GetMouseState(x, y) 0
#define SDL_WarpMouse(x, y) ((void)0)
#define SDL_ShowCursor(value) ((void)0)
#define SDL_WM_SetCaption(title, icon) ((void)0)
#endif
