/***************************************************************************
 * Original code from RockBox
 * was: apps/menus/theme_menu.c
 * Copyright (C) 2007 Jonathan Gordon
 * Portions Copyright (C) 2026 RockPod contributors
 * GNU General Public License (version 2+)
 *
 * Native appearance and interface preferences.
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "kernel.h"
#include "settings/settings.h"
#include "rbpaths.h"
#include "widgets/menu.h"
#include "ui/native_theme_runtime.h"
#include "dir.h"
#include "screens/browse/browser.h"
#include "exported_settings.h"
#include "widgets/splash.h"
#include "screens/context_menu.h"
#include "screens/context_menu_config.h"

MENUITEM_SETTING(scrollbar_item, &global_settings.scrollbar, NULL);
MENUITEM_SETTING(scrollbar_width, &global_settings.scrollbar_width, NULL);
MENUITEM_SETTING(volume_type, &global_settings.volume_type, NULL);
MENUITEM_SETTING(battery_display, &global_settings.battery_display, NULL);
MENUITEM_SETTING(show_disk_activity, &global_settings.show_disk_activity,
                 NULL);
MENUITEM_SETTING(show_background_tasks, &global_settings.show_background_tasks,
                 NULL);
MENUITEM_SETTING(show_working, &global_settings.show_working, NULL);
MAKE_MENU(activity_indicators_menu, ID2P(LANG_ACTIVITY_INDICATORS), NULL,
            Icon_NOICON,
            &show_disk_activity,
            &show_background_tasks,
            &show_working);

static const struct browse_folder_info fonts = {FONT_DIR, SHOW_FONT};

int browse_folder(void *param)
{
    const char *ext, *setting;
    int lang_id = -1;
    char selected[MAX_FILENAME+10];
    const struct browse_folder_info *info =
        (const struct browse_folder_info*)param;

    struct browse_context browse = {
        .dirfilter = info->show_options,
        .icon = Icon_NOICON,
        .root = info->dir,
    };

    if (!dir_exists(info->dir)) {
        splash(HZ, ID2P(LANG_PLAYLIST_DIRECTORY_ACCESS_ERROR));
        return GO_TO_PREVIOUS;
    }

    /* if we are in a special settings folder, center the current setting */
    switch(info->show_options)
    {
        case SHOW_LNG:
            ext = "lng";
            if (global_settings.lang_file[0])
                setting = global_settings.lang_file;
            else
                setting = "english";
            lang_id = LANG_LANGUAGE;
            break;
        case SHOW_FONT:
            ext = "fnt";
            setting = global_settings.font_file;
            lang_id = LANG_CUSTOM_FONT;
            break;
        default:
            ext = setting = NULL;
            break;
    }

    /* If we've found a file to center on, do it */
    if (setting)
    {
        /* if setting != NULL, ext is initialized */
        snprintf(selected, sizeof(selected), "%s.%s", setting, ext);
        browse.selected = selected;
        browse.icon = Icon_Questionmark;
        browse.title = str(lang_id);
    }

    browser_get_context()->browse = NULL;  /*bugfix - force root dir reload */
    return rockbox_browse(&browse);
}

MENUITEM_FUNCTION_W_PARAM(browse_fonts, 0, ID2P(LANG_CUSTOM_FONT),
                          browse_folder, (void*)&fonts, NULL, Icon_Font);

MENUITEM_SETTING(show_icons, &global_settings.show_icons, NULL);
MENUITEM_SETTING(sep_menu, &global_settings.list_separator_height, NULL);

/* Art rows use thumbnails from the shared library cache. */
MENUITEM_SETTING(db_albumart, &global_settings.db_albumart, NULL);
MENUITEM_SETTING(db_artistart, &global_settings.db_artistart, NULL);
MENUITEM_SETTING(db_bookart, &global_settings.db_bookart, NULL);
MENUITEM_SETTING(db_authorart, &global_settings.db_authorart, NULL);
MENUITEM_SETTING(db_ebookart, &global_settings.db_ebookart, NULL);
MENUITEM_SETTING(db_ebookauthorart, &global_settings.db_ebookauthorart, NULL);
MENUITEM_SETTING(db_podcastart, &global_settings.db_podcastart, NULL);

/* One list under two names: Appearance > Interface Elements > Art Rows, and
 * Appearance inside Library > Music. */
#define ART_ROWS_ITEMS &db_albumart, &db_artistart, &db_bookart, \
                       &db_authorart, &db_ebookart, &db_ebookauthorart, \
                       &db_podcastart
MAKE_MENU(art_rows_menu, ID2P(LANG_ART_ROWS), NULL, Icon_NOICON,
            ART_ROWS_ITEMS);
MAKE_MENU(music_appearance_menu, ID2P(LANG_APPEARANCE), NULL, Icon_NOICON,
            ART_ROWS_ITEMS);

MAKE_MENU(scrollbar_menu, ID2P(LANG_SCROLL_BAR), NULL, Icon_NOICON,
            &scrollbar_item,
            &scrollbar_width);

MAKE_MENU(elements_menu, ID2P(LANG_ELEMENTS), NULL, Icon_Display_menu,
            &art_rows_menu,
            &show_icons,
            &scrollbar_menu,
            &battery_display,
            &volume_type,
            &activity_indicators_menu,
            &sep_menu);

extern const struct menu_item_ex main_menu_config_item;   /* main_menu.c */
extern const struct menu_item_ex music_menu_config_item;  /* general_settings.c */

/* Menus: what each one shows. The rows a context menu can hide are listed by
   kind, a screen each. */
#define CONTEXT_MENU_ITEM(name, label, kind)                                \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(label),    \
                              context_menu_config, (void *)(kind),          \
                              NULL, Icon_NOICON)
CONTEXT_MENU_ITEM(ctxmenu_now_playing_item, LANG_NOW_PLAYING_MENU,
                  CONTEXT_KIND_NOW_PLAYING)
CONTEXT_MENU_ITEM(ctxmenu_track_item,  LANG_TRACK_MENU,  CONTEXT_KIND_TRACK)
CONTEXT_MENU_ITEM(ctxmenu_album_item,  LANG_ALBUM_MENU,  CONTEXT_KIND_ALBUM)
CONTEXT_MENU_ITEM(ctxmenu_artist_item, LANG_ARTIST_MENU, CONTEXT_KIND_ARTIST)
CONTEXT_MENU_ITEM(ctxmenu_folder_item, LANG_FOLDER_MENU, CONTEXT_KIND_FOLDER)
CONTEXT_MENU_ITEM(ctxmenu_audiobook_item, LANG_AUDIOBOOK_MENU,
                  CONTEXT_KIND_AUDIOBOOK)
CONTEXT_MENU_ITEM(ctxmenu_podcast_item, LANG_PODCAST_MENU,
                  CONTEXT_KIND_PODCAST)
CONTEXT_MENU_ITEM(ctxmenu_book_item,   LANG_BOOK_MENU,   CONTEXT_KIND_BOOK)
#undef CONTEXT_MENU_ITEM

MAKE_MENU(context_menus_menu, ID2P(LANG_CONTEXT_MENUS), NULL, Icon_NOICON,
            &ctxmenu_now_playing_item, &ctxmenu_track_item, &ctxmenu_album_item,
            &ctxmenu_artist_item, &ctxmenu_folder_item,
            &ctxmenu_audiobook_item, &ctxmenu_podcast_item, &ctxmenu_book_item);

MAKE_MENU(menus_menu, ID2P(LANG_MENUS), NULL, Icon_NOICON,
            &main_menu_config_item,
            &music_menu_config_item,
            &context_menus_menu);

/* Appearance: everything about how the player looks, filed by what a setting
   does rather than by the subsystem that implements it -- so the peak meter,
   the viewers and the root menu editor all sit here. */
MENUITEM_SETTING(animations, &global_settings.animations, NULL);
MENUITEM_SETTING(status_cover_tint, &global_settings.status_cover_tint, NULL);
MENUITEM_FUNCTION(native_theme_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_NATIVE_THEME),
                  ui_theme_choose, NULL, Icon_Config);

MAKE_MENU(theme_menu, ID2P(LANG_THEME_SETTINGS_MENU), NULL, Icon_Display_menu,
            &native_theme_item, &status_cover_tint);

MAKE_MENU(appearance_menu, ID2P(LANG_APPEARANCE),
            NULL, Icon_Wps,
            &browse_fonts,
            &elements_menu,
            &wps_appearance_menu,
            &carousel_appearance_menu,
            &scroll_settings_menu,
            &viewers_menu,
            &peak_meter_menu,
            &menus_menu,
            &animations);
