/* AstraPod XRick integration. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_XRICK_COMPAT_H
#define ASTRAPOD_XRICK_COMPAT_H

#include "xrick/system/system.h"

bool xrick_resources_valid(void);
bool xrick_resource_path(const char *name, size_t length);
bool xrick_usb_event(long event);
bool xrick_load_interrupted(void);
void xrick_redraw(void);

#endif
