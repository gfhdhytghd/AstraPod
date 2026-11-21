/* AstraPod foreground plugin integration. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_COMPAT_H
#define ASTRAPOD_ROCK_COMPAT_H

#include <stdbool.h>
#include "config.h"

#ifdef ASTRAPOD_ROCK_COMPAT
int rock_browser(void *unused);
int rock_open_from_browser(const char *path);
bool rock_ui_raw(void);
bool rock_usb_taken(void);
void rock_usb_takeover(void);
const unsigned char *rock_widget_text(const unsigned char *text);
#else
static inline void rock_usb_takeover(void) {}
static inline bool rock_ui_raw(void) { return false; }
#endif

#endif
