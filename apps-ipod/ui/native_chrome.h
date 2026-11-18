/* Native status bar. GPL version 2+. */
#ifndef PODBOX_NATIVE_CHROME_H
#define PODBOX_NATIVE_CHROME_H

#include <stdbool.h>
#include <stdint.h>

struct ui_theme;
struct ui_view_frame;
struct ui_album_palette;

struct native_chrome_state {
    int8_t battery, hour, minute;
    uint8_t charging, numeric, clock_24h, link, busy;
};

struct native_chrome {
    const struct ui_theme *theme;
    int status_font;
    int title_font;
    int icon_font;
    struct native_chrome_state state;
};

/* Borrow the view's theme snapshot and hold references on its status fonts.
 * Keep the snapshot alive until native_chrome_close(). */
bool native_chrome_open(struct native_chrome *chrome, const struct ui_theme *theme);
void native_chrome_close(struct native_chrome *chrome);

/* Refresh the retained status values and return whether the view changed. */
bool native_chrome_poll(struct native_chrome *chrome);
bool native_chrome_title_fits(const struct native_chrome *chrome, const char *title);
bool native_chrome_volume_visible(void);

/* Draw into the active framebuffer without flushing. A NULL palette uses the
 * static colour tokens. Bounds and clip are both in screen coordinates. */
void native_chrome_draw(const struct native_chrome *chrome, const char *title,
                        const struct ui_view_frame *frame,
                        const struct ui_album_palette *palette);

#endif
