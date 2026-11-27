/* The chrome of the lyrics page under style.lyrics=classic. GPL version 2+.
 * The status bar sits above the lyrics, and Now Playing's progress bar and
 * times sit below them at Now Playing's own place, so neither moves when the
 * page opens. The current line sits in the list's selection bar. */
#ifndef LYRICS_CHROME_H
#define LYRICS_CHROME_H

#include <stdbool.h>
#include "lcd.h"
#include "ui/native_theme.h"
#include "ui/native_chrome.h"
#include "ui/retained.h"

/* Now Playing's progress row: its top, and the bar and times inside it. */
#define LYRICS_CHROME_STRIP 203

struct lyrics_chrome {
    struct ui_theme theme;
    struct native_chrome status;
    fb_data rows[LCD_HEIGHT];
    fb_data band_fallback;
    unsigned time_fg;
    int time_font, status_height;
    char elapsed[16], remaining[16];
    bool status_ready;
};

/* Copy the theme and load the fonts; close releases them. */
void lyrics_chrome_open(struct lyrics_chrome *chrome,
                        const struct ui_theme *theme);
void lyrics_chrome_close(struct lyrics_chrome *chrome);
/* The colour of the page behind the lyrics at screen row y. */
static inline fb_data lyrics_chrome_row(const struct lyrics_chrome *chrome,
                                        int y)
{
    return chrome->rows[y];
}
/* Whether the status bar changed since the last poll. */
bool lyrics_chrome_poll(struct lyrics_chrome *chrome);
void lyrics_chrome_status(struct lyrics_chrome *chrome, struct ui_rect clip);
/* Format the times; whether either changed. */
bool lyrics_chrome_times(struct lyrics_chrome *chrome, long elapsed,
                         long length);
/* The bar's filled width for the position. */
int lyrics_chrome_fill(long elapsed, long length);
/* Milliseconds until the bar or either time next changes. */
long lyrics_chrome_next_ms(long elapsed, long length);
/* The progress row, over the restored page, clipped to rows of `clip`. */
void lyrics_chrome_strip(struct lyrics_chrome *chrome, int fill,
                         struct ui_rect clip);
/* The selection bar `height` rows tall from screen row `top`, only between
 * rows `from` and `to`. */
void lyrics_chrome_band(const struct lyrics_chrome *chrome, int top,
                        int height, int from, int to);

#endif
