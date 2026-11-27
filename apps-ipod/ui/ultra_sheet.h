/* PodBox Fruit Company Ultra: sheet motion. GPL version 2+. */
#ifndef PODBOX_UI_ULTRA_SHEET_H
#define PODBOX_UI_ULTRA_SHEET_H

#include <stdbool.h>
#include "sheet.h"

void ultra_sheet_begin(struct ui_sheet *sheet, bool open, const char *route);
/* Frame `frame` of the sheet's motion, or false to leave it to the stock
 * compositor. */
bool ultra_sheet_paint(struct ui_sheet *sheet, const fb_data *backdrop,
                       const fb_data *body, struct ui_rect bounds,
                       bool changed, fb_data *output, int width, int height,
                       void (*present)(void), unsigned frame);

#endif
