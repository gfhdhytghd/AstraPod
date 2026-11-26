/* Panel hardware scroll for native screens. GPL version 2+. */
#ifndef PODBOX_UI_HWSCROLL_H
#define PODBOX_UI_HWSCROLL_H

#include <stdbool.h>
#include <stdint.h>
#include "retained.h"

/* The panel moves the rows from `top` to the bottom of the screen by a
 * start line instead of a rewrite: a frame is that move plus a few bands
 * of rows sent from the framebuffer, all inside one refresh's blanking.
 * Only the iPod Classic's LCD type 1 does it; the simulator emulates the
 * panel and checks each frame against the framebuffer. */

#define UI_HWSCROLL_BANDS 8

struct ui_hwscroll_frame {
    int top;       /* first row of the scroll area */
    int shift;     /* rows the area's content moves up; negative moves down */
    int count;     /* bands in use */
    struct ui_rect band[UI_HWSCROLL_BANDS];
};

/* Sends a frame, and before it `other`, damage above `top` (width 0 for
 * none). The bands must cover every pixel of the area that differs from
 * the last frame's pixel `shift` rows further down. False when the frame
 * cannot go this way; nothing of it was sent, and the caller sends the
 * frame by update_rect(). On success the frame counts as one submission
 * in ui_flush_count() and ui-perf. */
bool ui_hwscroll_submit(const struct ui_hwscroll_frame *frame,
                        const struct ui_rect *other);

/* The TE rise of the refresh that first shows the last frame, while no
 * other transfer has reached the area or gone through ui_flush_dirty()
 * since. */
bool ui_hwscroll_latest(uint32_t *rise);

/* Call before any other submission: ui_hwscroll_latest() stops reporting. */
void ui_hwscroll_note_update(void);

/* Transfers so far that reach into the area, frames included. A frame is
 * only right on top of what its caller queued last: read this after
 * submitting, and send a frame only while it has not moved. */
unsigned int ui_hwscroll_writes(void);

/* The start line's distance from `top`, 0 when the panel is not scrolled. */
int ui_hwscroll_offset(void);

/* Every submission ends here (ui_note_flush()). The simulator can record
 * what the panel then shows. */
void ui_hwscroll_flushed(void);

/* Call once the panel shows the framebuffer whole, as after a page
 * transition's last frame. */
void ui_hwscroll_note_framebuffer(void);

/* How many times the panel has shown the framebuffer whole, and
 * ui_hwscroll_writes() just after the latest. */
unsigned int ui_hwscroll_framebuffers(unsigned int *writes);

#endif
