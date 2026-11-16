/* Cider shuffle/repeat pages for Now Playing long-presses. GPL version 2+. */
#ifndef CIDER_CONTROLS_H
#define CIDER_CONTROLS_H
#include <stdbool.h>
#include "config.h"
/* Row title and value text of a Cider mode (CIDER_MODE_*), localized. */
#ifdef HAVE_USB_PODLINK
const char *cider_mode_title(int mode);
const char *cider_mode_label(int mode, int value);
#else
/* Remote quick screens are unavailable when PodLink is disabled. */
static inline const char *cider_mode_title(int mode)
{ (void)mode; return ""; }
static inline const char *cider_mode_label(int mode, int value)
{ (void)mode; (void)value; return ""; }
#endif
/* Cider-bound native Now Playing menu; true when USB took the screen. */
bool cider_context_show(void);
int cider_context_destination(void);
bool cider_store_debug_show(void);
#endif
