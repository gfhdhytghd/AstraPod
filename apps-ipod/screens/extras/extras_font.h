/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Extras' large stock faces (14-helvetica-bold-calendar,
 * 18/21/23/29-helvetica-bold, 21-helvetica-bold-stopwatch) hold the stock
 * Latin glyphs only. Text that a translation can change goes through
 * extras_font_for(), which swaps in a theme font when the face lacks a
 * character.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_FONT_H
#define PODBOX_SCREENS_EXTRAS_FONT_H

/* `font` when it holds every character of `text`, else the theme's text
 * font nearest its height. The theme font stays loaded until the theme
 * changes; the caller never unloads it. */
int extras_font_for(int font, const char *text);

#endif
