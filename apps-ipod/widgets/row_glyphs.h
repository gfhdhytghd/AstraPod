/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The glyph a page menu's row draws in the theme's icon font, chosen by the
 * lang id of the row's label.
 ****************************************************************************/

#ifndef _ROW_GLYPHS_H_
#define _ROW_GLYPHS_H_

/* The codepoint of `lang_id`'s glyph in 24x24-icons.fnt, or 0 for none. */
int row_glyph(int lang_id);

#endif /* _ROW_GLYPHS_H_ */
