/* AstraPod foreground plugin integration. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_COMPAT_H
#define ASTRAPOD_ROCK_COMPAT_H

#include <stdbool.h>
#include "config.h"

#ifdef ASTRAPOD_ROCK_COMPAT
int rock_browser(void *unused);
/* The selected input path is borrowed until return. NULL starts the module. */
int rock_open_from_browser(const char *path, const char *file);
int rock_run_from_shell(const char *path);
bool rock_ui_raw(void);
bool rock_ui_native(void);
bool rock_usb_taken(void);
void rock_usb_takeover(void);
void rock_media_shutdown(void);
void rock_playback_takeover(void);
void rock_action_reset(void);
const unsigned char *rock_widget_text(const unsigned char *text);
#else
static inline void rock_usb_takeover(void) {}
static inline void rock_media_shutdown(void) {}
static inline void rock_playback_takeover(void) {}
static inline bool rock_ui_raw(void) { return false; }
static inline bool rock_ui_native(void) { return false; }
#endif

#endif
