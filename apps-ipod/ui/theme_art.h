#ifndef THEME_ART_H
#define THEME_ART_H

#include <stdbool.h>
#include "native_theme.h"
#include "stock_art.h"

/* Under style.art a theme draws the stock iPod Classic firmware's bitmaps
 * (stock_art.h) in place of its own shapes. Plastic is Classic drawn so. */
bool ui_theme_art(const struct ui_theme *theme);

/* The stock status bar battery for `percent` charge, in 23 steps. */
enum stock_art_id ui_art_battery(int percent);

/* Repeat `id` across the `width` columns from screen x, its top at y, as
 * stock lays a bar's tile: from the left, and with `flush` the last whole
 * tile drawn against the right end instead of cut there. */
void ui_art_tile(enum stock_art_id id, int x, int y, int width, bool flush,
                 struct ui_rect clip);

/* Stock's selection bar `height` rows tall, RGB888 from the top, for the
 * heights it draws; NULL for any other. */
const uint32_t *ui_art_selection(int height);

#endif
