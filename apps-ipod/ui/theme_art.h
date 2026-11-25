#ifndef THEME_ART_H
#define THEME_ART_H

#include "lcd.h"
#include "native_theme.h"

/* Bitmaps a theme draws in place of its own shapes under style.art. Plastic
 * is Apple's art from the stock iPod Classic firmware. */
enum ui_art_role {
    UI_ART_CHEVRON, UI_ART_PLAY, UI_ART_PAUSE, UI_ART_BATTERY_FULL,
    UI_ART_BATTERY_EMPTY, UI_ART_CHARGING, UI_ART_SHUFFLE,
    UI_ART_SPEAKER_QUIET, UI_ART_SPEAKER_LOUD, UI_ART_COUNT
};

/* alpha is 4bpp, 0 opaque to 15 clear, two pixels to a byte with the first
 * in the low nibble, rows packed with no padding. Art with no pixels is a
 * mask drawn in the foreground colour; art with no alpha is opaque. x and y
 * place it from the point its caller anchors it at. */
struct ui_art {
    uint8_t width, height;
    int8_t x, y;
    const fb_data *pixels;
    const unsigned char *alpha;
};

extern const struct ui_art plastic_art[UI_ART_COUNT];

/* The theme's art for `role`, or NULL when it draws its own. */
const struct ui_art *ui_theme_art(const struct ui_theme *theme,
                                  enum ui_art_role role);
/* Draw the left `width` columns of `art` anchored at (x, y) in the current
 * viewport. */
void ui_art_draw(const struct ui_art *art, int x, int y, int width);

#endif
