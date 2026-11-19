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
    uint8_t charging, numeric, clock_24h, busy, volume_visible, volume_percent;
    uint8_t playmode;
};

enum native_chrome_glyph {
    CHROME_PLAY, CHROME_PAUSE,
    CHROME_BATTERY_FIRST,
    CHROME_BATTERY_LAST = CHROME_BATTERY_FIRST + 6,
    CHROME_CHARGE_FIRST,
    CHROME_CHARGE_LAST = CHROME_CHARGE_FIRST + 3,
    CHROME_GLYPH_COUNT,
};

struct native_chrome_indicators {
    uint8_t ink[CHROME_GLYPH_COUNT]; /* 0 is invisible, 255 is full ink */
    int8_t battery;                /* numeric percent, or -1 if unknown */
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
bool native_chrome_volume_visible(void);
void native_chrome_indicators(const struct native_chrome_state *state,
                             struct native_chrome_indicators *out);

/* Draw into the active framebuffer without flushing. Now Playing supplies
 * mode glyphs for the left cell and draws its own volume overlay. A NULL
 * palette uses the static colours. Bounds and clip are in screen coordinates. */
void native_chrome_draw(const struct native_chrome *chrome, const char *left,
                        bool playback_modes,
                        const struct ui_view_frame *frame,
                        const struct ui_album_palette *palette,
                        const struct native_chrome_indicators *indicators);

#endif
