/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * A theme font for text that the Extras' Latin-only stock faces lack.
 ****************************************************************************/
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "font.h"
#include "rbunicode.h"
#include "settings/settings.h"
#include "ui/native_theme_runtime.h"
#include "extras_font.h"

/* The theme's text roles; together they span its sizes. */
static const enum ui_font_role roles[] = {
    UI_FONT_STATUS, UI_FONT_LIST, UI_FONT_TITLE, UI_FONT_WPS_TITLE,
};

/* One theme font per Latin-only face height; height 0 is a free entry. */
static struct { int height, font; } fallback[6];
static unsigned generation;

static bool has_text(int font, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    font_lock(font, true);
    struct font *pf = font_get(font);
    bool covered = true;
    while (*p && covered)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        covered = font_has_glyph(pf, ch);
    }
    font_lock(font, false);
    return covered;
}

static int nearest(int height)
{
    int best = -1, gap = 0;
    for (unsigned i = 0; i < ARRAYLEN(roles); i++)
    {
        int font = ui_theme_current_font_load(roles[i],
                                              global_settings.glyphs_to_cache);
        if (font < 0)
            continue;
        int g = abs(font_get(font)->height - height);
        if (best < 0 || g < gap)
        {
            font_unload(best);
            best = font;
            gap = g;
        }
        else
            font_unload(font);
    }
    return best;
}

int extras_font_for(int font, const char *text)
{
    if (font < 0 || has_text(font, text))
        return font;
    if (generation != ui_theme_generation())
    {
        for (unsigned i = 0; i < ARRAYLEN(fallback); i++)
        {
            if (fallback[i].height)
                font_unload(fallback[i].font);
            fallback[i].height = 0;
        }
        generation = ui_theme_generation();
    }
    int height = font_get(font)->height;
    unsigned free = ARRAYLEN(fallback);
    for (unsigned i = 0; i < ARRAYLEN(fallback); i++)
    {
        if (fallback[i].height == height)
            return fallback[i].font;
        if (!fallback[i].height)
            free = i;
    }
    int theme = nearest(height);
    if (theme < 0)
        return font;
    if (free == ARRAYLEN(fallback))
    {
        font_unload(theme);
        return font;
    }
    fallback[free].height = height;
    fallback[free].font = theme;
    return theme;
}
