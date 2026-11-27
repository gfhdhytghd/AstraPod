/* Foreground plugin display ownership. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_DISPLAY_H
#define ASTRAPOD_ROCK_DISPLAY_H

#include "draw/screen_access.h"
#include "fs_defines.h"
#include "ui/statusbar.h"

struct rock_display_state {
    struct viewport *viewport;
    struct viewport defaults;
    fb_data *backdrop;
    char title[MAX_PATH];
    enum themable_icons icon;
    bool persistent;
};

struct rock_native_scope {
    struct rock_display_state display;
    bool from_raw;
    unsigned previous_floor;
};

void rock_display_start(fb_data *caller_pixels, fb_data *widget_pixels);
void rock_display_stop(bool back_to_browser);
void rock_raw_lcd_update(void);
void rock_raw_lcd_update_rect(int x, int y, int w, int h);
void rock_theme_enable(enum screen_type screen, bool enable, struct viewport *vp);
void rock_theme_undo(enum screen_type screen, bool force);
bool rock_set_title(const char *title, enum themable_icons icon,
                    enum screen_type screen);
void rock_send_event(unsigned short id, void *data);

struct rock_native_scope rock_display_native_begin(void);
enum rock_display_resume {
    ROCK_RESUME_SILENT,
    ROCK_RESUME_PRESENT,
    ROCK_RESUME_DISCARD,
};
void rock_display_native_end(const struct rock_native_scope *scope,
                             enum rock_display_resume resume);

#endif
