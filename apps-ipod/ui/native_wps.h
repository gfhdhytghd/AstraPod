/* Retained local Now Playing. GPL version 2+. */
#ifndef PODBOX_UI_NATIVE_WPS_H
#define PODBOX_UI_NATIVE_WPS_H

#include <stdbool.h>

struct wps_state;

bool native_wps_open(const struct wps_state *state);
bool native_wps_active(void);
bool native_wps_refresh(const struct wps_state *state);
void native_wps_invalidate(void);
void native_wps_close(void);

#endif
