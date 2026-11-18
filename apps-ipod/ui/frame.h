/* Framebuffer damage and submission for native UI. GPL version 2+. */
#ifndef PODBOX_UI_FRAME_H
#define PODBOX_UI_FRAME_H

#include <stdbool.h>
#include "draw/screen_access.h"

#define UI_MAX_DIRTY_RECTS 1

struct ui_dirty_rect {
    int x, y, w, h;
};

void ui_mark_dirty(enum screen_type screen);
void ui_mark_dirty_rect(enum screen_type screen, int x, int y, int w, int h);
bool ui_take_dirty(enum screen_type screen);
int ui_take_dirty_rects(enum screen_type screen, struct ui_dirty_rect *out);
void ui_flush_dirty(void);

/* The active compositor can defer LCD updates and then take the damage. */
void ui_inhibit_flush(bool inhibit);
bool ui_flush_inhibited(void);

/* Scroll holds nest across dialogs. Returns true when none remain. */
bool ui_hold_scrolling(bool hold);
bool ui_scrolling_held(void);

void ui_note_render(unsigned int usec);
void ui_note_flush(unsigned int usec);
unsigned int ui_flush_count(void);
unsigned int ui_render_usec(void);
unsigned int ui_flush_usec(void);

#endif
