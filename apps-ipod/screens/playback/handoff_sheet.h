/* PodBox continuity handoff. GNU General Public License, version 2+. */
#ifndef PODBOX_HANDOFF_SHEET_H
#define PODBOX_HANDOFF_SHEET_H

#include <stdbool.h>

struct handoff_sheet {
    const char *title, *target;
    long accepted;
    unsigned long start;
    int reason;
    bool preparing, armed;
};

/* The callback advances the handoff and returns a dialog disposition. */
int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int));
void handoff_present_remote(void);

#endif
