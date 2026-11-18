#ifndef BUTTON_SCRIPT_SDL_H
#define BUTTON_SCRIPT_SDL_H

/* Simulator-only. Reads a text key script and optionally captures LCD frames. */

void button_script_sdl_set_path(const char *path);
void button_script_sdl_start(void);
void button_script_sdl_present(const void *rgb565, int pitch);

#endif
