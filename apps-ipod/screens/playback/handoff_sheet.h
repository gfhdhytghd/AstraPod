/* PodBox continuity handoff. GNU General Public License, version 2+. */
#ifndef PODBOX_HANDOFF_SHEET_H
#define PODBOX_HANDOFF_SHEET_H

#include <stdbool.h>

#define HANDOFF_FRAME_BYTES (2 * FRAMEBUFFER_SIZE)
#define HANDOFF_FONT_RESERVE (16 * 1024)

struct handoff_sheet {
    const char *title, *target;
    unsigned long start;
    int reason;
    bool preparing, armed;
};

/* The callback advances the handoff and returns a dialog disposition. */
int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int));
/* The UI thread releases the native sheet before entering a system modal. */
void handoff_sheet_interrupt(void);
void handoff_present_remote(void);

#endif
