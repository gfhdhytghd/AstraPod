/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/icon.c
 * Copyright (C) 2007 Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * Draws the built-in file and toolkit icons.
 ****************************************************************************/
#include "config.h"
#include "icon.h"
#include "screen_access.h"
#include "draw/viewport.h"
#include "speech/language.h"
#include "bitmaps/default_icons.h"

/* x,y in letters, not pixles */
void screen_put_icon(struct screen * display,
                       int x, int y, enum themable_icons icon)
{
    screen_put_icon_with_offset(display, x, y, 0, 0, icon);
}

void screen_put_icon_with_offset(struct screen * display,
                       int x, int y, int off_x, int off_y,
                       enum themable_icons icon)
{
    const int icon_width = get_icon_width(display->screen_type);
    const int icon_height = get_icon_height(display->screen_type);
    int xpos, ypos;
    int width, height;
    display->getstringsize((unsigned char *)"M", &width, &height);
    xpos = x*icon_width + off_x;
    ypos = y*height + off_y;

    if ( height > icon_height )/* center the cursor */
        ypos += (height - icon_height) / 2;
    screen_put_iconxy(display, xpos, ypos, icon);
}

/* x,y in pixels */
void screen_put_iconxy(struct screen * display,
                       int xpos, int ypos, enum themable_icons icon)
{
    const int width = get_icon_width(display->screen_type);
    const int height = get_icon_height(display->screen_type);
    const bool is_rtl = lang_is_rtl();

    if (icon <= Icon_NOICON)
    {
        if (is_rtl)
            xpos = display->getwidth() - xpos - width;
        screen_clear_area(display, xpos, ypos, width, height);
        return;
    }
    if (icon >= Icon_Last_Themeable)
        icon = Icon_Questionmark;

    if (is_rtl)
        xpos = display->getwidth() - xpos - width;


    display->bmp_part(&bm_default_icons, 0, height * icon, xpos, ypos, width, height);
}

void screen_put_cursorxy(struct screen * display, int x, int y, bool on)
{
    screen_put_icon(display, x, y, on?Icon_Cursor:0);
}

int get_icon_width(enum screen_type screen)
{
    (void)screen;
    return bm_default_icons.width;
}

int get_icon_height(enum screen_type screen)
{
    (void)screen;
    return bm_default_icons.height / Icon_Last_Themeable;
}

int get_icon_format(enum screen_type screen)
{
    (void)screen;
    return bm_default_icons.format;
}
