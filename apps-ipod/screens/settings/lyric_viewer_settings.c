/* Animated lyrics settings. GPL version 2 or later.
 * Geometry, font sizes and durations are part of the page design.
 * Legacy config keys remain readable, but are no longer offered as controls. */
#include "config.h"
#include "lang.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "draw/icon_bitmaps.h"
MENUITEM_SETTING(lyric_align, &global_settings.lyric_align, NULL);
MENUITEM_SETTING(lyric_colour_mode, &global_settings.lyric_colour_mode, NULL);
MENUITEM_SETTING(lyric_prev_opacity, &global_settings.lyric_prev_opacity, NULL);
MENUITEM_SETTING(lyric_next_opacity, &global_settings.lyric_next_opacity, NULL);
MENUITEM_SETTING(lyric_lead, &global_settings.lyric_lead, NULL);
MENUITEM_SETTING(lyric_highlight, &global_settings.lyric_highlight, NULL);
MENUITEM_SETTING(lyric_backlight, &global_settings.lyric_backlight, NULL);
MAKE_MENU(lyric_viewer_menu, ID2P(LANG_LYRICS_MENU), NULL, Icon_NOICON,
          &lyric_align, &lyric_colour_mode, &lyric_prev_opacity, &lyric_next_opacity,
          &lyric_lead, &lyric_highlight, &lyric_backlight);
