/* PodBox continuity handoff. GNU General Public License, version 2+. */
#ifndef PODBOX_HANDOFF_H
#define PODBOX_HANDOFF_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_USB_PODLINK
#include "podlink.h"
void handoff_init(void);
void handoff_reset(void);
bool handoff_receive(const struct podlink_message *message);
unsigned int handoff_next(char *record, unsigned int size);
bool handoff_capable(void);
bool handoff_hold_source(void);
bool handoff_prompt_wps(uint32_t usb_epoch);
void handoff_begin(void);
void handoff_usb_dismissed(void);
bool handoff_pending(void);
int handoff_step(void);
bool handoff_take_dissolve(void);
#else
static inline bool handoff_capable(void) { return false; }
static inline bool handoff_hold_source(void) { return false; }
static inline bool handoff_prompt_wps(uint32_t epoch) { (void)epoch; return false; }
static inline void handoff_begin(void) {}
static inline void handoff_usb_dismissed(void) {}
static inline bool handoff_pending(void) { return false; }
static inline int handoff_step(void) { return 0; }
#endif

#endif
