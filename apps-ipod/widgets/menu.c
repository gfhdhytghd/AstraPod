/***************************************************************************
 * Original code from RockBox
 * was: apps/menu.c
 * Copyright (C) 2002 Robert E. Hak
 * GNU General Public License (version 2+)
 *
 * The menu engine. Walks a static menu_item_ex tree, renders it as a list,
 * dispatches callbacks, and hands settings items to the option/value
 * screens.
 *
 * Menus are declared, not built. The MENUITEM_* macros in menu.h construct
 * const structs at file scope, so a menu costs no RAM and cannot be edited at
 * runtime -- what varies is decided by each item's callback, which can hide
 * the item (ACTION_EXIT_MENUITEM) or act when it is entered. Finding out what
 * a menu does means reading its callbacks.
 *
 * do_menu() is re-entered recursively for submenus, so one call sits on the
 * stack per level of menu depth.
 *
 * Parts, in order:
 *   - resolving an item's title, icon and voice id from its type
 *   - the list callbacks that present the menu as a list
 *   - item visibility and callback dispatch
 *   - do_menu(): the loop, including descent into submenus and settings
 ****************************************************************************/
/*
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "system/appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "button.h"
#include "button_chord.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "settings/settings_tags.h"
#include "option_select.h"
#include "speech/talk.h"
#include "lang.h"
#include "speech/language.h"            /* lang_id_to_english */
#include "system/activity.h"
#include "system/app_util.h"
#include "audio/sound_feedback.h"
#include "system/shutdown.h"
#include "input/action.h"
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif
#include "screens/settings/exported_settings.h"
#include "string.h"
#include "root_menu.h"
#include "audio.h"
#include "draw/viewport.h"
#include "dialog_prose.h"                  /* Explain */
#include "splash.h"
#include "screens/settings/settings_help.h"
#include "screens/settings/sound_settings.h"
#include "screens/playback/quick_screen.h"
#include "screens/shortcuts.h"
#include "ui/statusbar.h"

#include "draw/icon_bitmaps.h"

/* gui api */
#include "list.h"
#include "row_glyphs.h"
#include "ui/scene.h"
#include "ui/stock_art_ids.h"
#include "screens/fruit_menu.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;
static int talk_menu_item(int selected_item, void *data);

struct menu_data_t
{
    const struct menu_item_ex *menu;
    int selected;
};

static int empty_menu_callback(int action, const struct menu_item_ex *this_item, struct gui_synclist *this_list)
{
    return action;
    (void)this_item;
    (void)this_list;
}

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback)
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;

    if (!*menu_callback)
        *menu_callback = &empty_menu_callback;
}

int menu_item_event(int action, const struct menu_item_ex *item)
{
    menu_callback_type callback;
    get_menu_callback(item, &callback);
    return callback(action, item, NULL);
}

static bool query_audio_status(int *old_audio_status)
{
    bool redraw_list = false;
    /* query audio status to see if it changed */
    int new_audio_status = audio_status();
    if (*old_audio_status != new_audio_status)
    {  /* force a redraw if anything changed the audio status from outside */
        *old_audio_status = new_audio_status;
        redraw_list = true;
    }
    return redraw_list;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}
static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
static const char *item_label(const struct menu_item_ex *menu, int type,
                              int index, char *buffer, size_t buffer_len);

static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    const char *name;
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
        {
            name = menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        }
        else
            name = menu->strings[selected_item];

        if (P2ID((unsigned char *)name) > VOICEONLY_DELIMITER)
            name = "";

        return name;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];
    return item_label(menu, type, selected_item, buffer, buffer_len);
}

/* The name of `menu`, row `index` of a menu of `type`. */
static const char *item_label(const struct menu_item_ex *menu, int type,
                              int index, char *buffer, size_t buffer_len)
{
    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(index,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if (menu->flags & MENU_SETTING_LABEL)
        return P2STR(menu->callback_and_desc->desc);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v = find_setting(menu->variable);
        if (v)
            return str(v->lang_id);
        else return "Not Done yet!";
    }
    return P2STR(menu->callback_and_desc->desc);
}

/* The item a row stands for, as get_menu_item_name() resolves it. */
static const struct menu_item_ex *menu_row_item(int selected_item,
                                                const struct menu_item_ex *menu)
{
    if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        return menu->submenus[get_menu_selection(selected_item, menu)];
    return menu;
}

/* The setting a row changes, or NULL for any other row. */
static const struct settings_list *menu_row_setting(int selected_item,
                                                    const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);

    if (type == MT_RETURN_ID)
        return NULL;
    menu = menu_row_item(selected_item, menu);
    type = (menu->flags&MENU_TYPE_MASK);
    if (type != MT_SETTING && type != MT_SETTING_W_TEXT)
        return NULL;
    /* Volume Limit's entry is a custom setting, with no range to read. */
    if (menu->variable == &global_settings.volume_limit)
        return volume_limit_setting();
    return find_setting(menu->variable);
}

/* A function row's in-place value (see struct menu_function_settings), or
 * NULL for a row whose function opens a screen. */
static const char *function_value(const struct menu_item_ex *item, int *voice)
{
    const struct menu_function_settings *settings;
    void *param = NULL;

    switch (item->flags & MENU_TYPE_MASK)
    {
        case MT_FUNCTION_CALL:
            settings = &item->function->settings;
            break;
        case MT_FUNCTION_CALL_W_PARAM:
            settings = &item->function_param->settings;
            param = item->function_param->param;
            break;
        default:
            return NULL;
    }
    *voice = -1;
    return settings->value ? settings->value(param, voice) : NULL;
}

static const struct menu_row_presenter *row_presenter;
const struct menu_row_presenter *menu_set_row_presenter(const struct menu_row_presenter *presenter)
{
    const struct menu_row_presenter *previous = row_presenter;
    row_presenter = presenter;
    return previous;
}

/* A row presented other than by its setting: by the installed presenter, or
 * as one of Fruit Company's in-place Settings rows. */
static bool present_row(const struct menu_item_ex *item,
                        enum list_row_kind *kind, const char **value)
{
    return (row_presenter && row_presenter->describe(item, kind, value)) ||
           fruit_menu_describe(item, kind, value);
}

static enum list_row_kind menu_get_kind(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    const struct settings_list *setting;
    enum list_row_kind kind;
    const char *value;
    int voice;
    if (present_row(menu_row_item(selected_item, menu), &kind, &value))
        return kind;

    if ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID)
        return LIST_ROW_ACTION;

    switch (menu_row_item(selected_item, menu)->flags&MENU_TYPE_MASK)
    {
        case MT_MENU:
            return LIST_ROW_MENU;
        case MT_SETTING:
        case MT_SETTING_W_TEXT:
            setting = menu_row_setting(selected_item, menu);
            return option_steps_in_place(setting)
                   ? LIST_ROW_TOGGLE : LIST_ROW_SETTING;
        default:
            if (function_value(menu_row_item(selected_item, menu), &voice))
                return LIST_ROW_TOGGLE;
            return root_menu_item_kind(menu_row_item(selected_item, menu));
    }
}

/* A setting row's value as the setting's own screen words it. Only the
 * types option_value_as_int() reads have one: a filename or a custom setting
 * says nothing here. */
static const char* menu_get_value(int selected_item, void * data,
                                  char *buffer, size_t buffer_len)
{
    enum list_row_kind kind;
    const char *value;
    if (present_row(menu_row_item(selected_item, data), &kind, &value))
        return value;
    const struct settings_list *setting =
        menu_row_setting(selected_item, (const struct menu_item_ex *)data);
    int voice;

    if (!setting)
        return function_value(menu_row_item(selected_item, data), &voice);
    switch (setting->flags&F_T_MASK)
    {
        case F_T_BOOL:
        case F_T_INT:
        case F_T_UINT:
            break;
        default:
            return NULL;
    }

    /* option_get_valuestring() leaves the buffer alone for a type it does
     * not format. */
    buffer[0] = '\0';
    return option_get_valuestring(setting, buffer, buffer_len,
                                  option_value_as_int(setting));
}

/* The full settings tree, as the root Settings entry opens it. */
extern const struct menu_item_ex main_menu_;

/* The stock settings tile for what a row or a menu is named, in any
 * language. */
static const struct {
    int16_t lang_id;
    uint8_t art;
} pane_tiles[] = {
    { LANG_SOUND_SETTINGS, STOCK_ART_SETTINGS_SOUNDCHECKON },
    { LANG_REPLAYGAIN, STOCK_ART_SETTINGS_SOUNDCHECKON },
    { LANG_VOLUME_LIMIT, STOCK_ART_SETTINGS_VOLUMELIMIT },
    { LANG_PLAYBACK, STOCK_ART_SETTINGS_REPEATALL },
    { LANG_SPOKEN_WORD_SPEED, STOCK_ART_SETTINGS_AUDIOBOOKS },
    { LANG_SEGREGATE_AUDIOBOOKS, STOCK_ART_SETTINGS_AUDIOBOOKS },
    { LANG_LIBRARY, STOCK_ART_SETTINGS_SORTBY },
    { LANG_SORT_MENU, STOCK_ART_SETTINGS_SORTBY },
    { LANG_SORT_ALBUMS_BY_SHORT, STOCK_ART_SETTINGS_SORTBY },
    { LANG_THEME_SETTINGS_MENU, STOCK_ART_SETTINGS_BRIGHTNESS },
    { LANG_BRIGHTNESS, STOCK_ART_SETTINGS_BRIGHTNESS },
    { LANG_APPEARANCE, STOCK_ART_SETTINGS_MAINMENU },
    { LANG_MENUS, STOCK_ART_SETTINGS_MAINMENU },
    { LANG_MAIN_MENU_SETTINGS, STOCK_ART_SETTINGS_MAINMENU },
    { LANG_MUSIC_MENU_SETTINGS, STOCK_ART_SETTINGS_MAINMENU },
    { LANG_BATTERY_POWER, STOCK_ART_SETTINGS_BACKLIGHT_ON },
    { LANG_SYSTEM, STOCK_ART_SETTINGS_CLICKEROFF },
    { LANG_KEYCLICK, STOCK_ART_SETTINGS_CLICKERON },
    { LANG_MANAGE_MENU, STOCK_ART_SETTINGS_RESET },
    { LANG_RESET, STOCK_ART_SETTINGS_RESET },
    { LANG_SETTINGS_CHANGED, STOCK_ART_SETTINGS_RESET },
    { LANG_FRUIT_VOLUME_LIMIT, STOCK_ART_SETTINGS_VOLUMELIMIT },
    { LANG_FRUIT_SORT_BY, STOCK_ART_SETTINGS_SORTBY },
    { LANG_AUDIOBOOKS, STOCK_ART_SETTINGS_AUDIOBOOKS },
};

static int backlight_tile(int timeout)
{
    static const struct { int8_t up_to; uint8_t art; } steps[] = {
        { 2, STOCK_ART_SETTINGS_BACKLIGHT_2S },
        { 5, STOCK_ART_SETTINGS_BACKLIGHT_5S },
        { 10, STOCK_ART_SETTINGS_BACKLIGHT_10S },
        { 20, STOCK_ART_SETTINGS_BACKLIGHT_20S },
    };
    /* -1 is Off and 0 Always On. */
    if (timeout <= 0)
        return timeout < 0 ? STOCK_ART_SETTINGS_BACKLIGHT_OFF :
                             STOCK_ART_SETTINGS_BACKLIGHT_ON;
    for (unsigned i = 0; i < ARRAYLEN(steps); i++)
        if (timeout <= steps[i].up_to)
            return steps[i].art;
    return STOCK_ART_SETTINGS_BACKLIGHT_30S;
}

/* A tile that shows a setting's value, as stock's Shuffle, Repeat, Sound
 * Check, Clicker and Backlight do, or -1. */
static int value_tile(const void *variable)
{
    if (variable == &global_settings.shuffle_mode)
    {
        switch (global_settings.shuffle_mode)
        {
            case SHUFFLE_OFF: return STOCK_ART_SETTINGS_SHUFFLEOFF;
            case SHUFFLE_SONGS: return STOCK_ART_SETTINGS_SHUFFLESONGS;
            /* The existing stock art set has no Albums tile. */
            default: return -1;
        }
    }
    if (variable == &global_settings.repeat_mode)
        return global_settings.repeat_mode == REPEAT_OFF ?
                   STOCK_ART_SETTINGS_REPEATOFF :
               global_settings.repeat_mode == REPEAT_ONE ?
                   STOCK_ART_SETTINGS_REPEATONE : STOCK_ART_SETTINGS_REPEATALL;
    if (variable == &global_settings.replaygain_settings.type)
        return global_settings.replaygain_settings.type == REPLAYGAIN_OFF ?
               STOCK_ART_SETTINGS_SOUNDCHECKOFF :
               STOCK_ART_SETTINGS_SOUNDCHECKON;
    if (variable == &global_settings.keyclick)
        return global_settings.keyclick ? STOCK_ART_SETTINGS_CLICKERON :
                                          STOCK_ART_SETTINGS_CLICKEROFF;
    if (variable == &global_settings.keyclick_hardware)
        return global_settings.keyclick_hardware ?
               STOCK_ART_SETTINGS_CLICKERON : STOCK_ART_SETTINGS_CLICKEROFF;
    if (variable == &global_settings.backlight_timeout)
        return backlight_tile(global_settings.backlight_timeout);
    if (variable == &global_settings.backlight_timeout_plugged)
        return backlight_tile(global_settings.backlight_timeout_plugged);
    return -1;
}

/* The language id an item is named by, or -1 for a name of its own. */
static int item_lang_id(const struct menu_item_ex *item)
{
    int type = item->flags & MENU_TYPE_MASK;
    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *setting = find_setting(item->variable);
        return setting ? setting->lang_id : -1;
    }
    if (!(item->flags & MENU_HAS_DESC) || (item->flags & MENU_DYNAMIC_DESC))
        return -1;
    return P2ID(item->callback_and_desc->desc);
}

/* The tile for an item of its own, or -1. */
static int item_tile(const struct menu_item_ex *item)
{
    int type = item->flags & MENU_TYPE_MASK;
    int art = type == MT_SETTING || type == MT_SETTING_W_TEXT ?
              value_tile(item->variable) : -1;
    int lang_id = item_lang_id(item);
    if (lang_id == LANG_BACKLIGHT)
        return backlight_tile(global_settings.backlight_timeout);
    if (lang_id == LANG_FRUIT_SOUND_CHECK)
        return value_tile(&global_settings.replaygain_settings.type);
    for (unsigned i = 0; art < 0 && i < ARRAYLEN(pane_tiles); i++)
        if (pane_tiles[i].lang_id == lang_id)
            art = pane_tiles[i].art;
    return art;
}

/* The tile of the nearest menu from `menu` up to `tree` that has one,
 * `inherited` when none below `tree` has; -2 when `menu` is not in `tree`. */
static int branch_tile(const struct menu_item_ex *tree,
                       const struct menu_item_ex *menu, int inherited,
                       int depth)
{
    int own = item_tile(tree);
    if (own >= 0)
        inherited = own;
    if (tree == menu)
        return inherited;
    if (depth > 12)
        return -2;
    for (unsigned i = 0; i < MENU_GET_COUNT(tree->flags); i++)
    {
        const struct menu_item_ex *sub = tree->submenus[i];
        if (!sub || (sub->flags & MENU_TYPE_MASK) != MT_MENU)
            continue;
        int art = branch_tile(sub, menu, inherited, depth + 1);
        if (art != -2)
            return art;
    }
    return -2;
}

/* A row with no tile of its own takes its menu's, or the one its menu
 * inherits from its branch of Settings. */
static int menu_tile(const struct menu_item_ex *menu)
{
    int art = branch_tile(&main_menu_, menu, STOCK_ART_SETTINGS_MAINMENU, 0);
    if (art < 0)
        art = item_tile(menu);
    return art < 0 ? STOCK_ART_SETTINGS_MAINMENU : art;
}

static enum list_pane_kind menu_get_pane(int selected_item, void * data,
                                         int *art)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    const struct menu_item_ex *item = menu_row_item(selected_item, menu);

    if (menu == &root_menu_ && (menu->flags&MENU_TYPE_MASK) == MT_MENU)
        return root_menu_item_pane(item, art);
    if (fruit_menu_owns(menu))
        return fruit_menu_item_pane(menu, item, art);
    if (item != menu && item_lang_id(item) == LANG_ABOUT)
        return LIST_PANE_DEVICE;
    *art = item != menu ? item_tile(item) : -1;
    if (*art < 0)
        *art = menu_tile(menu);
    return LIST_PANE_INFO;
}

static int menu_get_range(int selected_item, void * data)
{
    const struct settings_list *setting =
        menu_row_setting(selected_item, (const struct menu_item_ex *)data);

    return setting ? option_value_position(setting) : -1;
}

static enum themable_icons  menu_get_icon(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int menu_icon = Icon_NOICON;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
    {
        return Icon_Menu_functioncall;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags&MENU_HAS_DESC)
        menu_icon = menu->callback_and_desc->icon_id;
    else if (menu->flags&MENU_DYNAMIC_DESC)
        menu_icon = menu->menu_get_name_and_icon->icon_id;

    /* An item that named no icon of its own gets the generic one for its type.
     * Every MT_* has to appear here: a type missing from the chain keeps
     * Icon_NOICON and draws a blank where its neighbours draw an icon, whatever
     * iconset the theme supplies. MT_FUNCTION_CALL_W_PARAM was the missing one
     * -- it left the six colour setters, Manage Settings' four entries, Browse
     * EQ Presets, Save Current Playlist, Properties and Track Info iconless. */
    if (menu_icon == Icon_NOICON)
    {
        unsigned int flags = (menu->flags&MENU_TYPE_MASK);
        if(flags == MT_MENU)
            menu_icon = Icon_Submenu;
        else if (flags == MT_SETTING || flags == MT_SETTING_W_TEXT)
             menu_icon = Icon_Menu_setting;
        else if (flags == MT_FUNCTION_CALL
              || flags == MT_FUNCTION_CALL_W_PARAM
              || flags == MT_RETURN_VALUE)
             menu_icon = Icon_Menu_functioncall;
    }
    return menu_icon;
}

/* The glyph `item` draws as a row of a page menu: the one its definition
 * names with ICON_GLYPH(), else the one row_glyphs.c gives its label, else
 * 0. A string list's row is `index` of `item`. */
int menu_item_glyph(const struct menu_item_ex *item, int index)
{
    int type = item->flags & MENU_TYPE_MASK;
    int icon = Icon_NOICON;
    int id = -1;

    if (type == MT_RETURN_ID)
        return (item->flags & MENU_DYNAMIC_DESC) ? 0 :
               row_glyph(P2ID((unsigned char *)item->strings[index]));
    if (item->flags & MENU_HAS_DESC)
        icon = item->callback_and_desc->icon_id;
    else if (item->flags & MENU_DYNAMIC_DESC)
        icon = item->menu_get_name_and_icon->icon_id;
    if (ICON_IS_GLYPH(icon))
        return ICON_GLYPH_CODE(icon);

    if (item->flags & MENU_SETTING_LABEL)
        id = P2ID(item->callback_and_desc->desc);
    else if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *setting = find_setting(item->variable);
        id = setting ? setting->lang_id : -1;
    }
    else if (item->flags & MENU_HAS_DESC)
        id = P2ID(item->callback_and_desc->desc);
    return id >= 0 ? row_glyph(id) : 0;
}

/* A page menu's row: its own glyph, else the icon for its type. */
static enum themable_icons menu_get_page_icon(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int index = get_menu_selection(selected_item, menu);
    int glyph = (menu->flags & MENU_TYPE_MASK) == MT_MENU ?
                menu_item_glyph(menu->submenus[index], 0) :
                menu_item_glyph(menu, index);

    return glyph ? ICON_GLYPH(glyph) : menu_get_icon(selected_item, data);
}

bool menu_page_list(const struct gui_synclist *list)
{
    return list->callback_get_item_icon == menu_get_page_icon;
}

static char* init_title(const struct menu_item_ex *menu, int *icon,
                        char* buf, size_t buf_sz)
{
    char *title;

    if (menu->flags&MENU_HAS_DESC)
    {
        *icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        *icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, buf, buf_sz);
    }
    else
    {
        *icon = Icon_NOICON;
        title = "";
    }

    if (*icon == Icon_NOICON)
        *icon = Icon_Submenu_Entered;

    return title;
}

/* True if this row is hidden at the current Settings Mode.
 *
 * Two sources, because there are two kinds of row. A setting says so through
 * settings_tags.c, which keeps that table the single place deciding what is
 * advanced -- there is nothing to mark here and nothing to fall out of step.
 * A submenu or an action has no settings_list entry to tag, so it carries
 * MENU_ADVANCED itself.
 *
 * A submenu with nothing left to show hides too. That means marking every row
 * of a screen advanced hides the screen, without anyone having to remember to
 * mark the screen as well -- and it is why Peak Meter and Artwork Filter
 * disappear whole rather than becoming empty lists. */
static bool item_hidden(const struct menu_item_ex *item, int depth)
{
    int type;

    if (!item || depth > MAX_MENUS)
        return false;
    if (item->flags & MENU_ADVANCED)
        return true;

    type = item->flags & MENU_TYPE_MASK;

    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *setting = find_setting(item->variable);
        return setting && (settings_tags_get(setting) & TAG_ADVANCED);
    }

    if (type == MT_MENU)
    {
        int count = MENU_GET_COUNT(item->flags);
        for (int i = 0; i < count; i++)
            if (!item_hidden(item->submenus[i], depth + 1))
                return false;
        return count > 0;
    }

    return false;
}

/* A submenu row's page as it opens: its title and its rows, leaving out
 * those Settings Mode hides. A row's own menu callback can hide it too,
 * which this does not ask. */
static bool menu_child_texts(int selected_item, void *data, int rows,
                             unsigned token, list_child_text *text,
                             void *context)
{
    (void)rows;
    (void)token;
    static char buf[MAX_PATH];
    const struct menu_item_ex *menu = data;
    if ((menu->flags & MENU_TYPE_MASK) != MT_MENU)
        return true;
    const struct menu_item_ex *sub =
        menu->submenus[get_menu_selection(selected_item, menu)];
    int type = sub->flags & MENU_TYPE_MASK;
    if (type != MT_MENU && type != MT_RETURN_ID)
        return true;

    int count = MIN(MENU_GET_COUNT(sub->flags), MAX_MENU_SUBITEMS);
    for (int i = 0; i < count; i++)
    {
        const char *name;
        if (type == MT_MENU)
        {
            if (!menu_item_in_settings_mode(sub->submenus[i]))
                continue;
            name = item_label(sub->submenus[i], type, i, buf, sizeof(buf));
        }
        else if (sub->flags & MENU_DYNAMIC_DESC)
            name = sub->menu_get_name_and_icon->list_get_name(
                i, sub->menu_get_name_and_icon->list_get_name_data,
                buf, sizeof(buf));
        else
            name = sub->strings[i];

        if (name && P2ID((const unsigned char *)name) <= VOICEONLY_DELIMITER)
            text(i, P2STR((const unsigned char *)name), context);
    }
    int icon;
    text(-1, init_title(sub, &icon, buf, sizeof(buf)), context);
    return true;
}

bool menu_item_in_settings_mode(const struct menu_item_ex *item)
{
    return global_settings.settings_mode != SETTINGS_MODE_STANDARD ||
           !item_hidden(item, 0);
}

/* Show the explanation for a setting, or say there is not one.
 *
 * Offered on every setting rather than only where a stanza exists, which would
 * mean reading the file before the context menu opens -- on every context
 * press. The 5G powers its ATA interface down after seven idle seconds, so
 * that turns a button press into a possible disk spin-up. The file is read
 * only when someone actually asks. */
static bool explain(const char *key, const char *title)
{
    /* Static rather than automatic: the dialog below runs a whole screen of
     * its own from this frame, so a kilobyte here sits under all of it.
     * One explanation is on screen at a time, so one buffer is enough. A
     * stanza longer than this is cut short without a sign, so
     * tools/check-settings-docs.sh checks against this size. */
    static char text[1024];

    if (!key || !settings_help_lookup(key, text, sizeof text))
    {
        splash(HZ, ID2P(LANG_NO_EXPLANATION));
        return false;
    }

    return dialog_prose_show(title, text);
}

bool menu_explain_setting(const struct settings_list *setting)
{
    return explain(setting->cfg_name,
                   setting->lang_id != -1 ? (const char *)str(setting->lang_id)
                                          : setting->cfg_name);
}

/* An action row -- Rebuild Database, Clear Backdrop -- has no settings_list
 * entry and so no cfg name to key its help by. It is keyed by its own label
 * instead, under an "action: " namespace so the two cannot collide:
 *
 *     [action: Rebuild Database]
 *
 * The label rather than something more stable because it is the only identity
 * such a row has at runtime. Renaming a row therefore orphans its stanza, which
 * shows as Explain saying there is nothing rather than as anything worse.
 * The key is the English label whatever the language, as the file is keyed. */
static bool explain_action(const unsigned char *desc)
{
    const char *label = P2STR(desc);
    char key[96];

    if (!label)
    {
        splash(HZ, ID2P(LANG_NO_EXPLANATION));
        return false;
    }

    snprintf(key, sizeof key, "action: %s",
             P2ID(desc) != -1 ? lang_id_to_english(P2ID(desc)) : label);
    return explain(key, label);
}

/* A top row asked for by whoever is about to open a menu, consumed once by the
 * next list build.
 *
 * A request rather than an argument because do_menu()'s start_selected is used
 * by most of its callers to *restore* a position, and those want the list's
 * usual habit of keeping a row visible above the cursor. Only a menu opening
 * deliberately past its own first row wants that row scrolled away, and it is
 * the caller who knows. Same shape as browser_db.c's pending_top_item, for the
 * same reason. */
static int pending_top_item = -1;

void menu_set_pending_top_item(int item)
{
    pending_top_item = item;
}

static const struct menu_row_hiding *row_hiding;

void menu_set_row_hiding(const struct menu_row_hiding *hiding)
{
    row_hiding = hiding;
}

static int init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS], char* buf, size_t buf_sz)
{
    if (!menu || !lists)
    {
        panicf("init_menu_lists, NULL pointer");
        return 0;
    }

    int i;
    int start_action = ACTION_ENTER_MENUITEM;
    int count = MIN(MENU_GET_COUNT(menu->flags), MAX_MENU_SUBITEMS);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = &empty_menu_callback;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
        {
            /* A string list indexes by position and holds no items, so there
               is nothing to hide there. */
            if (!fruit_menu_is_stock(menu) &&
                !menu_item_in_settings_mode(menu->submenus[i]))
                continue;
            if (row_hiding && (menu->flags & MENU_EXITAFTERTHISMENU)
                && row_hiding->hidden(menu->submenus[i]))
                continue;

            get_menu_callback(menu->submenus[i],&menu_callback);
        }

        if (menu_callback(ACTION_REQUEST_MENUITEM,
            type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i], lists)
                != ACTION_EXIT_MENUITEM)
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }

    current_submenus_menu = (struct menu_item_ex *)menu;

    title = init_title(menu, &icon, buf, buf_sz);
    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_row_callbacks(lists, menu_get_kind, menu_get_value, NULL);
    gui_synclist_set_range_callback(lists, menu_get_range);
    /* A menu that is a page of its own, not a sheet or a plugin's. */
    enum current_activity activity = get_current_activity();
    bool page = activity != ACTIVITY_CONTEXTMENU && activity != ACTIVITY_PLUGIN;
    gui_synclist_set_icon_callback(lists, !global_settings.show_icons ? NULL :
                                   page ? menu_get_page_icon : menu_get_icon);
    if (page)
    {
        gui_synclist_set_pane_callback(lists, menu_get_pane);
        if (!parent)
            gui_synclist_set_child_texts_callback(lists, menu_child_texts);
    }
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(lists, talk_menu_item);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    gui_synclist_select_item(lists, find_menu_selection(selected));

    /* After the selection, which sets a top row of its own. */
    if (pending_top_item >= 0)
    {
        gui_synclist_set_top_item(lists, pending_top_item);
        pending_top_item = -1;
    }

    get_menu_callback(menu,&menu_callback);
    if (callback)
        start_action = menu_callback(start_action, menu, lists);

    return start_action;
}

static int talk_menu_item(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int id = -1;
    int type;
    unsigned char *str;
    int sel = get_menu_selection(selected_item, menu);

        if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        {
            type = menu->submenus[sel]->flags&MENU_TYPE_MASK;
            if (menu->submenus[sel]->flags & MENU_SETTING_LABEL)
                talk_id(P2ID(menu->submenus[sel]->callback_and_desc->desc),
                        false);
            else if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
                talk_setting(menu->submenus[sel]->variable);
            else
            {
                if (menu->submenus[sel]->flags&(MENU_DYNAMIC_DESC))
                {
                    int (*list_speak_item)(int selected_item, void * data)
                        = menu->submenus[sel]->menu_get_name_and_icon->
                        list_speak_item;
                    if(list_speak_item)
                        list_speak_item(sel, menu->submenus[sel]->
                                        menu_get_name_and_icon->
                                        list_get_name_data);
                    else
                    {
                        char buffer[80];
                        str = menu->submenus[sel]->menu_get_name_and_icon->
                            list_get_name(sel, menu->submenus[sel]->
                                    menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
                        id = P2ID(str);
                        if (id == -1 && str)
                            talk_text((char *)str, false);
                    }
                }
                else
                    id = P2ID(menu->submenus[sel]->callback_and_desc->desc);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        else if(((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID))
        {
            if ((menu->flags&MENU_DYNAMIC_DESC) == 0)
            {
                unsigned char *s = (unsigned char *)menu->strings[sel];
                /* string list, try to talk it if ID2P was used */
                id = P2ID(s);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        return 0;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}


void do_setting_from_menu(const struct menu_item_ex *temp,
                          struct viewport parent[NB_SCREENS])
{
    char *title;
    if (!temp)
    {
        panicf("do_setting_from_menu, NULL pointer");
        return;
    }
    const struct settings_list *setting = find_setting(temp->variable);

    if ((temp->flags&MENU_TYPE_MASK) == MT_SETTING_W_TEXT)
        title = temp->callback_and_desc->desc;
    else
        title = ID2P(setting->lang_id);

    do_setting_screen(setting, title, parent);
}

/* Open a setting from outside any menu -- the settings search, which finds an
 * item by walking the tree rather than by the user standing on it.
 *
 * do_setting_screen() alone is not enough. A setting whose effect is applied
 * by its menu callback rather than by the setting itself -- Directory Cache
 * enabling the cache, Charge During USB calling usb_charging_enable(), the
 * sleep timer restarting a running countdown -- would change value and do
 * nothing. So this reproduces what the menu loop does around a row: the same
 * ENTER/EXIT pair, on the same item, in the same order.
 *
 * Calling the item's own callback rather than keeping a list of which settings
 * need what is the point: there is nothing here to fall out of step, and a
 * setting given a callback tomorrow is handled without anyone remembering
 * this function exists.
 *
 * `this_list` is NULL, which every callback in the tree already tolerates --
 * none reads it. */
void do_setting_from_menu_standalone(const struct menu_item_ex *item,
                                     struct viewport parent[NB_SCREENS])
{
    menu_callback_type cb;

    if (!item)
        return;

    get_menu_callback(item, &cb);

    /* A callback may refuse the item on entry, exactly as in a menu. */
    if (cb(ACTION_ENTER_MENUITEM, item, NULL) == ACTION_EXIT_MENUITEM)
        return;

    do_setting_from_menu(item, parent);

    cb(ACTION_EXIT_MENUITEM, item, NULL);
}

static struct {
    struct ui_scene *scene;
    const struct menu_item_ex *menu, *peer;
} next_scene;

void menu_link_scene(struct ui_scene *scene, const struct menu_item_ex *menu,
                     const struct menu_item_ex *peer)
{
    next_scene.scene = scene;
    next_scene.menu = menu;
    next_scene.peer = peer;
}

/* display a menu */
void menu_paint(const struct menu_item_ex *menu, int selected)
{
    char buf[80];
    int icon;
    struct gui_synclist lists;
    int top = pending_top_item;
    char *title = init_title(menu, &icon, buf, sizeof buf);
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, true, NULL);
    }
    init_menu_lists(menu, &lists, selected, false, NULL, buf, sizeof buf);
    pending_top_item = top;
    gui_synclist_draw_settled(&lists);
    gui_synclist_scroll_stop(&lists);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
}

int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    enum current_activity activity = get_current_activity();
    bool context_sheet = !parent && activity == ACTIVITY_CONTEXTMENU;
    bool animate = !context_sheet && !parent && !hide_theme && activity != ACTIVITY_PLUGIN;
    struct ui_scene modal = {0};
    struct ui_scene local_scene = {0};
    struct ui_scene *scene = &local_scene;
    const struct menu_item_ex *peer = NULL;
    if (animate && next_scene.scene && next_scene.menu == start_menu)
    {
        scene = next_scene.scene;
        peer = next_scene.peer;
    }
    next_scene.scene = NULL;
    if (!peer && !ui_scene_automatic())
        ui_scene_interrupt();
    if (context_sheet)
        ui_scene_open_modal(&modal, "context-up");
    bool scene_handoff = false;
    int selected = start_selected? *start_selected : 0;
    int ret = 0;
    int action;
    int start_action;
    int icon;
    char buf[80], *title;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;

    bool in_stringlist, done = false;
    bool redraw_lists;
    /* A setting, or a function row, Select stepped in place, whose new
     * value the redraw says */
    const struct settings_list *stepped = NULL;
    const struct menu_item_ex *stepped_row = NULL;

    int old_audio_status = audio_status();

    title = init_title(menu, &icon, buf, sizeof buf);
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, !hide_theme, NULL);
    }
    struct menu_data_t mstack[MAX_MENUS]; /* menu, selected */
    int stack_top = 0;

    struct viewport *vps = NULL;
    menu_callback_type menu_callback = &empty_menu_callback;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen)
     * start_action allows an action to be processed
     * by menu logic by bypassing get_action on the initial run */
    start_action = init_menu_lists(menu, &lists, selected, true, parent,
                                   buf, sizeof buf);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    gui_synclist_draw_settled(&lists);
    ui_scene_start(&modal);
    ui_scene_start(scene);
    gui_synclist_speak_item(&lists);

    while (!done)
    {
        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);

        if (UNLIKELY(start_action != ACTION_ENTER_MENUITEM))
        {
            action = start_action;
            start_action = ACTION_ENTER_MENUITEM;
        }
        else
        {
            /* HZ so the status bar redraws corectly */
            action = get_action(CONTEXT_MAINMENU|ALLOW_SOFTLOCK,
                                ui_scene_timeout(&modal, ui_scene_timeout(scene, list_do_action_timeout(&lists, HZ))));
        }
        if (action == ACTION_NONE && !gui_synclist_moving(&lists))
            lcd_update();

#ifdef HAVE_WHEEL_WPS_CHORD
        /* Root CANCEL normally stays at Home. A deferred screen shortcut
         * must instead return to root_menu's dispatcher, which consumes it.
         * Otherwise get_action keeps returning CANCEL without a queue wait. */
        if ((menu == &root_menu_ || menu == fruit_menu_root()) &&
            agents_nav_pending() != AGENTS_NAV_NONE)
        {
            ret = agents_nav_pending() == AGENTS_NAV_WPS ? GO_TO_WPS : GO_TO_PREVIOUS;
            done = true;
            break;
        }
#endif
        /* query audio status to see if it changed */
        redraw_lists = query_audio_status(&old_audio_status);


        if (menu == &root_menu_)
        {
            const struct menu_item_ex *row = current_subitems_count
                ? menu->submenus[get_menu_selection(lists.selected_item, menu)]
                : NULL;
            if (root_menu_refresh())
            {
                int selection = 0;
                for (unsigned int i = 0; i < MENU_GET_COUNT(menu->flags); i++)
                    if (menu->submenus[i] == row) selection = i;
                init_menu_lists(menu, &lists, selection, false, vps,
                                buf, sizeof buf);
                redraw_lists = true;
                if (row && menu->submenus[selection] != row)
                    action = ACTION_NONE;
            }
        }

        int new_action = menu_callback(action, menu, &lists);
        if (new_action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            ret = MENU_SELECTED_EXIT; /* exit after return from selection */
        else if (new_action == ACTION_REDRAW)
            redraw_lists = true;
        else
            action = new_action;

        if (LIKELY(gui_synclist_do_button(&lists, &action)))
        {
            ui_scene_update(&modal, true);
            ui_scene_update(scene, true);
            continue;
        }

        /* Only stack movement and normal list updates share the snapshots.
         * Other actions can hand the LCD to an unrelated screen. */
        bool settings_to_home = peer == &root_menu_;
        bool top_settings = settings_to_home && stack_top == 0;
        if (action != ACTION_NONE && action != ACTION_UNKNOWN && action != ACTION_REDRAW &&
            action != ACTION_STD_OK && action != ACTION_STD_CANCEL &&
            !(settings_to_home && action == ACTION_STD_MENU))
            ui_scene_cancel(scene);
        /* In the stock tree Menu goes back one level, as stock's does */
        if (action == ACTION_STD_MENU && start_menu == fruit_menu_root())
            action = ACTION_STD_CANCEL;
        if (action == ACTION_STD_QUICKSCREEN)
        {
            if (global_settings.shortcuts_replaces_qs ||
                quick_screen_current(action, NULL) == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
            {
                int last_screen = global_status.last_screen;
                global_status.last_screen = GO_TO_SHORTCUTMENU;
                int shortcut_ret = do_shortcut_menu(NULL);
                if (shortcut_ret == GO_TO_PREVIOUS)
                    global_status.last_screen = last_screen;
                else
                {
                    ret = shortcut_ret;
                    done = true;
                }
            }
            if (!done)
                init_menu_lists(menu, &lists, lists.selected_item, false, vps,
                                buf, sizeof buf);
            redraw_lists = true;
        }
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            redraw_lists = list_stop_handler();
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            if (menu == &root_menu_)
            {
                ret = GO_TO_ROOTITEM_CONTEXT;
                done = true;
            }
            else if (!in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                /* find_setting() answers NULL for a row whose variable has
                   no settings_list entry -- a setting deleted without its
                   menu item going with it, which is the failure this tree
                   is already shaped to expect. Every arm of the menu below
                   dereferences it, and is_setting_quickscreenable() calls
                   NULL quickscreenable, so it is the long menu that opens.
                   Offer nothing rather than the choice of eight faults. */
                const struct settings_list *ctx_setting =
                        (type == MT_SETTING_W_TEXT || type == MT_SETTING) ?
                            find_setting(temp->variable) : NULL;

                /* A context menu's own row offers only to be hidden */
                bool hide_row = type != -1 && row_hiding
                                && (menu->flags & MENU_EXITAFTERTHISMENU)
                                && row_hiding->hideable(temp);

                if (hide_row)
                {
                    MENUITEM_STRINGLIST(row_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_HIDE_THIS_ROW));

                    if (do_menu(&row_op_menu, NULL, NULL, false) == 0)
                        row_hiding->hide(temp);
                    /* Built again below, without the row */
                    current_submenus_menu = NULL;
                    redraw_lists = true;
                }
                else if (ctx_setting)
                {
                    const struct menu_item_ex *context_menu;
                    const struct settings_list *setting = ctx_setting;

                    /* Explain first, Reset second, and that order is load
                       bearing: non_quickscreen_op_menu below reuses the first
                       N of these strings, so anything a non-quickscreenable
                       setting must still offer has to come before the
                       quickscreen entries. */
                    MENUITEM_STRINGLIST(settings_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_EXPLAIN),
                                        ID2P(LANG_RESET_SETTING),
                                        ID2P(LANG_TOP_QS_ITEM),
                                        ID2P(LANG_LEFT_QS_ITEM),
                                        ID2P(LANG_BOTTOM_QS_ITEM),
                                        ID2P(LANG_RIGHT_QS_ITEM),
                                        ID2P(LANG_ADD_CURRENT_TO_FAVES),
                                        ID2P(LANG_ADD_TO_FAVES));

                    /* re-use the strings and desc from the settings_op_menu */
                    static const struct menu_item_ex non_quickscreen_op_menu =
                    {MT_RETURN_ID|MENU_HAS_DESC|MENU_ITEM_COUNT(2),
                    { .strings = settings_op_menu_},
                    {.callback_and_desc = &settings_op_menu__}};

                    if (is_setting_quickscreenable(setting))
                        context_menu = &settings_op_menu;
                    else
                    {
                        context_menu = &non_quickscreen_op_menu;
                    }
                    int msel = do_menu(context_menu, NULL, NULL, false);

                    switch (msel)
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* explain */
                            /* MENU in the explanation means the root menu,
                               not back to this row's context. */
                            if (menu_explain_setting(setting))
                            {
                                ret = GO_TO_ROOT;
                                done = true;
                            }
                            break;
                        case 1: /* reset setting */
                            reset_setting_by_user(setting);
                            settings_save();
                            settings_apply(false);
                            break;
                        case 2: /* set as top QS item */
                            global_settings.qs_items[QUICKSCREEN_TOP] = setting;
                            break;
                        case 3: /* set as left QS item */
                            global_settings.qs_items[QUICKSCREEN_LEFT] = setting;
                            break;
                        case 4: /* set as bottom QS item */
                            global_settings.qs_items[QUICKSCREEN_BOTTOM] = setting;
                            break;
                        case 5: /* set as right QS item */
                            global_settings.qs_items[QUICKSCREEN_RIGHT] = setting;
                            break;
                        case 6: /* Add current value of setting to faves.
                                   Same limitation on which can be
                                   added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING_APPLY, (void*)setting);
                            break;
                        case 7: /* Add to faves. Same limitation on which can be
                                  added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING, (void*)setting);
                            break;
                    } /* switch(do_menu()) */
                    if (menu->flags & MENU_EXITAFTERTHISMENU)
                        done = true; /* in case context_menu_show menu contains setting */
                    redraw_lists = true;
                }
                else if ((type == MT_FUNCTION_CALL
                          || type == MT_FUNCTION_CALL_W_PARAM)
                         && (temp->flags & MENU_HAS_DESC)
                         && !(menu->flags & MENU_EXITAFTERTHISMENU))
                {
                    /* Explain only. There is nothing to reset on an action and
                       nothing the quickscreen could do with one. Requires a
                       fixed label, so the dynamic-text rows are left out --
                       their text is built per draw and is nobody's key. A
                       context menu's rows are left out too: none of them has
                       an explanation to show. */
                    MENUITEM_STRINGLIST(action_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_EXPLAIN));

                    if (do_menu(&action_op_menu, NULL, NULL, false) == 0
                        && explain_action(temp->callback_and_desc->desc))
                    {
                        ret = GO_TO_ROOT;
                        done = true;
                    }

                    redraw_lists = true;
                }

                /* Rebuild this menu's row mapping, which the context menu we
                   just ran overwrote -- current_subitems is one global shared
                   by every do_menu. Without it the rows redraw unfiltered, so
                   an advanced row hidden by Standard comes back after a
                   context press and opens the wrong screen. */
                if (current_submenus_menu != menu)
                    init_menu_lists(menu, &lists, selected, true, vps,
                                    buf, sizeof buf);
            } /* else if (!in_stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            if (menu != &root_menu_)
            {
                ret = GO_TO_ROOT;
                if (settings_to_home)
                    scene_handoff = ui_scene_capture(scene, menu, peer, false);
            }
            else
                ret = GO_TO_PREVIOUS;
            done = true;
        }
        else if (action == ACTION_STD_CANCEL)
        {
            if (top_settings)
            {
                scene_handoff = ui_scene_capture(scene, menu, peer, false);
                if (!scene_handoff)
                    ui_scene_cancel(scene);
            }
            else if (!(animate && stack_top > 0 &&
                       ui_scene_capture(scene, menu, mstack[stack_top - 1].menu, false)))
                ui_scene_cancel(scene);
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);

            bool exiting_menu = false;
            in_stringlist = false;

            menu_callback(ACTION_EXIT_MENUITEM, menu, &lists);

            /* A context menu's submenu goes back to the menu it was opened
             * from; only the menu itself closes */
            if ((menu->flags & MENU_EXITAFTERTHISMENU) && !action_menu_back() && stack_top == 0)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;

            if (stack_top > 0)
            {
                stack_top--;
                menu = mstack[stack_top].menu;
                int msel = mstack[stack_top].selected;
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU) && !action_menu_back())
                    done = true;
                else
                    init_menu_lists(menu, &lists, msel, false, vps, buf, sizeof buf);
                redraw_lists = true;
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_ && menu != fruit_menu_root())
            {
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            redraw_lists = true;

            int type = (menu->flags&MENU_TYPE_MASK);
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
            {
                if (!current_subitems_count)
                {
                    ui_scene_cancel(scene);
                    continue;
                }
                temp = menu->submenus[selected];
            }
            else if (!in_stringlist)
                type = -1;

            bool to_settings = false;
            if (!in_stringlist && temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                to_settings = peer && menu == &root_menu_ && type == MT_RETURN_VALUE &&
                              temp->value == GO_TO_MAINMENU;
                get_menu_callback(temp, &menu_callback);
                if (type != MT_MENU && type != MT_RETURN_ID && !to_settings)
                    ui_scene_cancel(scene);
                action = menu_callback(ACTION_ENTER_MENUITEM, temp, &lists);
                if (action == ACTION_EXIT_MENUITEM)
                    break;
            }
            if (!to_settings && !(animate && stack_top < MAX_MENUS && temp &&
                (type == MT_MENU || (type == MT_RETURN_ID && !in_stringlist)) &&
                ui_scene_capture(scene, menu, temp, true)))
                ui_scene_cancel(scene);

            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, true, vps, buf, sizeof buf);
                    }
                    break;
                case MT_FUNCTION_CALL_W_PARAM:
                case MT_FUNCTION_CALL:
                {
                    int return_value, voice;
                    enum current_activity parent_activity = get_current_activity();
                    if (function_value(temp, &voice))
                        stepped_row = temp;
                    if (type == MT_FUNCTION_CALL_W_PARAM)
                    {
                        return_value = temp->function_param->function_w_param(
                                    temp->function_param->param);
                    }
                    else
                    {
                        return_value = temp->function->function();
                    }
                    /* Some native pages pop the context activity because
                     * their original caller always exited after the action. */
                    if (action_menu_back() && get_current_activity() != parent_activity)
                        push_current_activity(parent_activity);
                    if (action_menu_back() || !(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        /* Reload menu but don't run the calback again FS#8117 */
                        init_menu_lists(menu, &lists, selected, false, vps,
                                        buf, sizeof buf);
                    }
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    const struct settings_list *setting =
                        find_setting(temp->variable);
                    if (fruit_menu_step(temp))
                        ;
                    else if (option_steps_in_place(setting))
                    {
                        option_step_in_place(setting);
                        stepped = setting;
                    }
                    else
                        do_setting_from_menu(temp, vps);
                    init_menu_lists(menu, &lists, selected, false, vps, buf, sizeof buf);
                    redraw_lists = true;

                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        init_menu_lists(menu, &lists, 0, false, vps, buf, sizeof buf);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    ret = temp->value;
                    if (to_settings)
                        scene_handoff = ui_scene_capture(scene, menu, peer, true);
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (type != MT_MENU)
            {
                menu_callback(ACTION_EXIT_MENUITEM, temp, &lists);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu, &lists,selected, true, vps, buf, sizeof buf);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) && !action_menu_back() &&
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
        }
        else
        {
            int previous = get_menu_selection(gui_synclist_get_sel_pos(&lists),
                                              menu);
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case BUTTON_REDRAW:
                    /* A Shell action can open a nested menu. Restore the
                     * caller's row map and apply any new setting filters. */
                    init_menu_lists(menu, &lists, previous, false, vps,
                                    buf, sizeof buf);
                    redraw_lists = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        if (redraw_lists && !done)
        {
            const struct settings_list *spoken = stepped;
            const struct menu_item_ex *spoken_row = stepped_row;
            stepped = NULL;
            stepped_row = NULL;
            if (menu_callback(ACTION_REDRAW, menu, &lists) != ACTION_REDRAW)
            {
                ui_scene_cancel(scene);
                continue;
            }


            gui_synclist_set_title(&lists, lists.title, lists.title_icon);
            gui_synclist_draw_settled(&lists);
            if (scene->capturing)
                ui_scene_start(scene);
            else
                ui_scene_update(scene, true);
            ui_scene_update(&modal, true);
            if (spoken && global_settings.talk_menu)
                option_talk_value(spoken, option_value_as_int(spoken), false);
            else if (spoken_row && global_settings.talk_menu)
            {
                int voice;
                const char *value = function_value(spoken_row, &voice);
                if (value && voice >= 0)
                    talk_id(voice, false);
                else if (value)
                    talk_text(value, false);
            }
            else if (!spoken && !spoken_row)
                gui_synclist_speak_item(&lists);
        }
        else if (!scene_handoff)
        {
            ui_scene_update(&modal, false);
            ui_scene_update(scene, false);
        }
    }
    if (!scene_handoff)
        ui_scene_cancel(scene);

    keyclick_set_callback(NULL, NULL);
    /* The Agents shortcut leaves the loop after get_action() without
     * gui_synclist_do_button(), and `lists` is about to go out of scope */
    gui_synclist_forget(&lists);

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = mstack[0].menu;
            init_menu_lists(menu, &lists, mstack[0].selected, true, vps,
                            buf, sizeof buf);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    ui_scene_close_modal(&modal, ret != MENU_ATTACHED_USB, "context-down");
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(lists.title, lists.title_icon, i);
        viewportmanager_theme_undo(i, false);
    }
    return ret;
}
