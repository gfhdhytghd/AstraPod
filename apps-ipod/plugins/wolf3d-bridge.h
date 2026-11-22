/* Wolf3D's foreground SDL adapter. GPL-2.0-or-later. */
#ifndef WOLF3D_BRIDGE_H
#define WOLF3D_BRIDGE_H

void *wolf_malloc(size_t bytes);
void *wolf_calloc(size_t count, size_t bytes);
void *wolf_realloc(void *ptr, size_t bytes);
void wolf_free(void *ptr);

void wolf_service(void);
void wolf_error(const char *message) NORETURN_ATTR;
void wolf_allow_config_save(void);
bool wolf_can_save_config(void);

unsigned int wolf_audio_rate(void);
unsigned int wolf_open_audio(void);
void wolf_audio_pump(void);

#endif
