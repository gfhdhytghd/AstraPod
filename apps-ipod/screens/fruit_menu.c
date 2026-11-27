/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Fruit Company's menu tree: the stock iPod Classic 1.1.2 main menu, Music,
 * Videos, Photos, Extras and Settings, each row backed by the AstraPod screen
 * or setting that does the same job. AstraPod's own Settings tree is
 * Settings > Advanced Settings, and its System menu is in Extras.
 *
 * A row deeper than the main menu runs its screen from inside the menu, so
 * that leaving the screen, by Menu as much as by Back, comes back to the row
 * it was opened from, as on the stock firmware; root_menu_run_screen() keeps
 * the main menu's bookkeeping for it.
 *
 * Parts: the leaves, Music, Videos, Photos, Extras, Settings with its two
 * checklists, the main menu, the pane.
 ****************************************************************************/

#include <stdbool.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "playlist/fruit_otg.h"
#include "lang.h"
#include "kernel.h"
#include "audio.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "input/action.h"
#include "widgets/menu.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "root_menu.h"
#include "database/tagcache.h"
#include "screens/browse/app_list.h"
#include "screens/extras/extras.h"
#include "screens/extras/extras_calendars.h"
#include "screens/extras/extras_clock.h"
#include "ui/menu_pane.h"
#include "timefuncs.h"
#include "screens/browse/browser_db.h"
#include "screens/settings/exported_settings.h"
#include "screens/settings/sound_settings.h"
#include "screens/system/fruit_about.h"
#include "widgets/option_select.h"
#include "system/volume.h"
#include "screens/settings/eq_editor.h"
#include "widgets/yesno.h"
#include "ui/native_theme.h"
#include "ui/native_theme_runtime.h"
#include "ui/stock_art_ids.h"
#include "fruit_menu.h"
#include "ui/scene.h"

bool fruit_menu_active(void)
{
    return ui_theme_style(UI_STYLE_MENU_TREE) == UI_MENU_TREE_STOCK;
}

/* A screen run from a row: 0 stays in the menu, anything else leaves it. */
static int run(int screen)
{
    return root_menu_run_screen(screen);
}

/* A stock row whose stock screen AstraPod has nothing behind */
static int not_available(void)
{
    splash(HZ * 2, ID2P(LANG_FRUIT_NOT_AVAILABLE));
    return 0;
}

/* A stock list AstraPod keeps nothing for: Select does nothing, as on an
 * empty stock list. */
static int empty(void)
{
    return 0;
}

/* A setting's own picker, from a row that shows no value, as stock's do */
static int pick(const void *variable, int title)
{
    option_screen(find_setting(variable), NULL, false, ID2P(title));
    return 0;
}

/** Leaves **/

static int fruit_library(enum fruit_browse_kind kind, int title)
{
    if (browser_db_fruit_count(kind) == 0)
        return 0;
    return root_menu_library_row(BROWSER_DB_MENU_FRUIT, str(title));
}

static int artists(void)
{
    return fruit_library(FRUIT_BROWSE_ARTISTS, LANG_ARTISTS);
}
static int albums(void)
{
    return fruit_library(FRUIT_BROWSE_ALBUMS, LANG_ALBUMS);
}
static int genres(void)
{
    return fruit_library(FRUIT_BROWSE_GENRES, LANG_FRUIT_GENRES);
}
static int composers(void)
{
    return fruit_library(FRUIT_BROWSE_COMPOSERS, LANG_FRUIT_COMPOSERS);
}
static int compilations(void)
{
    return fruit_library(FRUIT_BROWSE_COMPILATIONS, LANG_FRUIT_COMPILATIONS);
}
static int audiobooks(void)
{
    return fruit_library(FRUIT_BROWSE_AUDIOBOOKS, LANG_AUDIOBOOKS);
}
static int songs(void)
{
    return fruit_library(FRUIT_BROWSE_SONGS, LANG_FRUIT_SONGS);
}
static int podcasts(void)
{
    return fruit_library(FRUIT_BROWSE_PODCASTS, LANG_PODCASTS);
}
static int itunes_u(void)
{
    return fruit_library(FRUIT_BROWSE_ITUNES_U, LANG_FRUIT_ITUNES_U);
}

static int cover_flow(void)   { return run(GO_TO_ALBUM_COVERS); }
static int playlists(void)    { return run(GO_TO_PLAYLISTS_SCREEN); }
static int search(void)       { return run(GO_TO_DB_SEARCH); }
static int photos(void)       { return run(GO_TO_IMAGES); }
static int now_playing(void)  { return run(GO_TO_WPS); }
static int movies(void)       { return run(GO_TO_VIDEOS); }

/* Music Quiz is a screen of the main menu's, not a plugin: the list leaves
 * for it, and comes back once it is done */
static int app_list(enum app_list_kind kind)
{
    int ret;
    while ((ret = app_list_screen(kind)) == GO_TO_MUSIC_QUIZ)
        if ((ret = run(GO_TO_MUSIC_QUIZ)))
            return ret;
    return ret == GO_TO_PREVIOUS || ret == GO_TO_ROOT ? 0 : ret;
}

static int games(void)        { return app_list(APP_LIST_GAMES); }
static int other_apps(void)   { return app_list(APP_LIST_OTHER); }
static int demos(void)        { return app_list(APP_LIST_DEMOS); }
static int files(void)        { return run(GO_TO_FILEBROWSER); }
static int playback_report(void) { return run(GO_TO_SPUN); }

/* AstraPod's sleep timer menu, the stock Sleep row */
extern const struct menu_item_ex sleep_timer_menu;
static int sleep_timer(void)
{
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "sleep-menu-push");
    int ret = do_menu(&sleep_timer_menu, NULL, NULL, false);
    ui_scene_close_modal(&scene, ret != MENU_ATTACHED_USB, "sleep-menu-pop");
    return ret == MENU_ATTACHED_USB ? ret : 0;
}

/* Not static in their files */
int time_screen(void *ignored);

/* Every song in the library, shuffled, as the stock main menu's row does. */
static int shuffle_songs(void)
{
    static char path[MAX_PATH];
    struct tagcache_search tcs;
    struct playlist_insert_context context;
    if (!tagcache_is_usable() || !tagcache_search(&tcs, tag_filename))
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return 0;
    }
    browser_db_fruit_music_filter(&tcs);
    if (playlist_create(NULL, NULL) < 0 ||
        playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                       false, false) < 0)
    {
        playlist_insert_context_release(&context);
        tagcache_search_finish(&tcs);
        return 0;
    }
    cpu_boost(true);
    int added = 0;
    while (tagcache_get_next(&tcs, path, sizeof(path)) &&
           playlist_insert_context_add(&context, tcs.result) >= 0)
        added++;
    playlist_insert_context_release(&context);
    tagcache_search_finish(&tcs);
    cpu_boost(false);
    if (!added)
        return 0;
    playlist_shuffle_songs(current_tick, -1);
    playlist_start(0, 0, 0);
    return run(GO_TO_WPS);
}

/* A row of a menu: name, what it runs, and the callback that hides it. */
#define ROW(name, lang, func, cb) \
    MENUITEM_FUNCTION(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), func, cb, \
                      Icon_NOICON)
#define MENU(name, lang, cb, rows) \
    static const struct menu_callback_with_desc name##_desc = { \
        cb, ID2P(lang), Icon_NOICON }; \
    static const struct menu_item_ex name = { \
        MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(ARRAYLEN(rows)), \
        { (void *)rows }, { .callback_and_desc = &name##_desc } }

/* One row of a Main Menu or Music Menu checklist, in stock order */
struct check_row {
    int16_t lang;
    bool indent;
};

/* Which bit of a checklist's mask turns `item` on, or -1 */
struct row_bit {
    const struct menu_item_ex *item;
    int8_t bit;
};

static int bit_of(const struct row_bit *rows, int count,
                  const struct menu_item_ex *item)
{
    for (int i = 0; i < count; i++)
        if (rows[i].item == item)
            return rows[i].bit;
    return -1;
}

/** Music **/

enum {
    MUSIC_COVER_FLOW, MUSIC_PLAYLISTS, MUSIC_ARTISTS, MUSIC_ALBUMS,
    MUSIC_COMPILATIONS, MUSIC_SONGS, MUSIC_RADIO, MUSIC_GENRES,
    MUSIC_COMPOSERS, MUSIC_AUDIOBOOKS, MUSIC_SEARCH, MUSIC_ROWS
};

static const struct check_row music_checks[MUSIC_ROWS] = {
    { LANG_COVER_FLOW_TITLE, false }, { LANG_PLAYLISTS, false },
    { LANG_ARTISTS, false }, { LANG_ALBUMS, false },
    { LANG_FRUIT_COMPILATIONS, false }, { LANG_FRUIT_SONGS, false },
    { LANG_FRUIT_RADIO, false }, { LANG_FRUIT_GENRES, false },
    { LANG_FRUIT_COMPOSERS, false }, { LANG_AUDIOBOOKS, false },
    { LANG_SEARCH, false },
};

static int music_callback(int action, const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list);

static int genius_mixes(void)
{
    return fruit_genius_show(true) == GO_TO_ROOT ? GO_TO_ROOT : 0;
}

ROW(music_cover_flow, LANG_COVER_FLOW_TITLE, cover_flow, music_callback);
ROW(music_genius_mixes, LANG_FRUIT_GENIUS_MIXES, genius_mixes, music_callback);
ROW(music_playlists, LANG_PLAYLISTS, playlists, music_callback);
ROW(music_artists, LANG_ARTISTS, artists, music_callback);
ROW(music_albums, LANG_ALBUMS, albums, music_callback);
ROW(music_compilations, LANG_FRUIT_COMPILATIONS, compilations, music_callback);
ROW(music_songs, LANG_FRUIT_SONGS, songs, music_callback);
ROW(music_genres, LANG_FRUIT_GENRES, genres, music_callback);
ROW(music_composers, LANG_FRUIT_COMPOSERS, composers, music_callback);
ROW(music_audiobooks, LANG_AUDIOBOOKS, audiobooks, music_callback);
ROW(music_search, LANG_SEARCH, search, music_callback);

/* Radio has no row: stock shows it only with the radio remote attached. */
static const struct row_bit music_bits[] = {
    { &music_cover_flow, MUSIC_COVER_FLOW }, { &music_playlists, MUSIC_PLAYLISTS },
    { &music_artists, MUSIC_ARTISTS }, { &music_albums, MUSIC_ALBUMS },
    { &music_compilations, MUSIC_COMPILATIONS }, { &music_songs, MUSIC_SONGS },
    { &music_genres, MUSIC_GENRES }, { &music_composers, MUSIC_COMPOSERS },
    { &music_audiobooks, MUSIC_AUDIOBOOKS }, { &music_search, MUSIC_SEARCH },
};

static int music_callback(int action, const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM)
    {
        int bit = bit_of(music_bits, ARRAYLEN(music_bits), this_item);
        if (bit >= 0 && !(global_settings.fruit_music_menu & (1u << bit)))
            return ACTION_EXIT_MENUITEM;
    }
    return action;
}

static const struct menu_item_ex *const music_rows[] = {
    &music_cover_flow, &music_genius_mixes, &music_playlists,
    &music_artists, &music_albums,
    &music_compilations, &music_songs, &music_genres, &music_composers,
    &music_audiobooks, &music_search,
};

static int home_callback(int action, const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

MENU(music_menu_fruit, LANG_MUSIC_BROWSER, home_callback, music_rows);

/** Videos **/

ROW(movies_row, LANG_FRUIT_MOVIES, movies, NULL);
ROW(rentals_row, LANG_FRUIT_RENTALS, empty, NULL);
ROW(tv_shows_row, LANG_FRUIT_TV_SHOWS, empty, NULL);
ROW(music_videos_row, LANG_FRUIT_MUSIC_VIDEOS, empty, NULL);
ROW(video_playlists_row, LANG_FRUIT_VIDEO_PLAYLISTS, empty, NULL);
ROW(video_settings_row, LANG_SETTINGS, not_available, NULL);

static const struct menu_item_ex *const videos_rows[] = {
    &movies_row, &rentals_row, &tv_shows_row, &music_videos_row,
    &video_playlists_row, &video_settings_row,
};

MENU(videos_menu, LANG_FRUIT_VIDEOS, home_callback, videos_rows);

/** Photos **/

ROW(all_photos_row, LANG_FRUIT_ALL_PHOTOS, photos, NULL);
ROW(photo_settings_row, LANG_SETTINGS, not_available, NULL);

static const struct menu_item_ex *const photos_rows[] = {
    &all_photos_row, &photo_settings_row,
};

MENU(photos_menu, LANG_FRUIT_PHOTOS, home_callback, photos_rows);

/** Extras **/

/* Stock Extras rows with no AstraPod screen behind them yet */
/* An Extras item's own way out: 0 back to the menu, or a GO_TO_* code */
static int extra(int (*item)(void))
{
    int ret = item();
    if (ret == GO_TO_ROOT)
        return MENU_ATTACHED_USB;
    return ret == GO_TO_PREVIOUS ? 0 : ret;
}

static int clocks(void)       { return extra(extras_clock); }
static int calendars(void)    { return extra(extras_calendars); }
static int contacts(void)     { return extra(extras_contacts); }
static int alarms(void)       { return extra(extras_alarms); }
static int notes(void)        { return extra(extras_notes); }
static int screen_lock(void)  { return extra(extras_screen_lock); }
static int stopwatch(void)    { return extra(extras_stopwatch); }

ROW(extras_clocks_row, LANG_FRUIT_CLOCKS, clocks, NULL);
ROW(extras_calendars_row, LANG_FRUIT_CALENDARS, calendars, NULL);
ROW(extras_contacts_row, LANG_FRUIT_CONTACTS, contacts, NULL);
ROW(extras_alarms_row, LANG_FRUIT_ALARMS, alarms, NULL);
ROW(extras_games_row, LANG_PLUGIN_GAMES, games, NULL);
ROW(extras_notes_row, LANG_FRUIT_NOTES, notes, NULL);
ROW(extras_screen_lock_row, LANG_FRUIT_SCREEN_LOCK, screen_lock, NULL);
ROW(extras_stopwatch_row, LANG_FRUIT_STOPWATCH, stopwatch, NULL);
ROW(extras_other_apps_row, LANG_APP_LIST_OTHER, other_apps, NULL);
ROW(extras_demos_row, LANG_PLUGIN_DEMOS, demos, NULL);
ROW(extras_files_row, LANG_DIR_BROWSER, files, NULL);

/* Not static in main_menu.c: AstraPod's System menu */
extern const struct menu_item_ex info_menu;

/* Stock's rows in stock's order, then AstraPod's own */
static const struct menu_item_ex *const extras_rows[] = {
    &extras_clocks_row, &extras_calendars_row, &extras_contacts_row,
    &extras_alarms_row, &extras_games_row, &extras_notes_row,
    &extras_screen_lock_row, &extras_stopwatch_row, &extras_other_apps_row,
    &extras_demos_row, &extras_files_row, &info_menu,
};

MENU(extras_menu, LANG_FRUIT_EXTRAS, home_callback, extras_rows);

/** Settings **/

static int show_about(void)
{
    int ret = fruit_about_screen();
    return ret == GO_TO_ROOT ? MENU_ATTACHED_USB : ret;
}

static int volume_limit(void)
{
    option_screen(volume_limit_setting(), NULL, false,
                  ID2P(LANG_FRUIT_VOLUME_LIMIT));
    setvol();
    return 0;
}

static int backlight(void)
{
    return pick(&global_settings.backlight_timeout, LANG_BACKLIGHT);
}

static int brightness(void)
{
    return pick(&global_settings.brightness, LANG_BRIGHTNESS);
}

static int audiobook_speed(void)
{
    return pick(&global_settings.spoken_speed, LANG_AUDIOBOOKS);
}

/* Leaving a screen for Home, as AstraPod's Menu does, is back to the row
 * here, as stock's Menu is. */
static int set_date_time(void)
{
    int ret = time_screen(NULL);
    return ret == MENU_ATTACHED_USB ? ret : 0;
}

static int eq_presets(void)
{
    return eq_presets_screen() == MENU_ATTACHED_USB ? MENU_ATTACHED_USB : 0;
}

/* AstraPod's own Settings tree */
static int advanced_settings(void)
{
    return run(GO_TO_MAINMENU);
}

static int legal(void)
{
    main_menu_show_license();
    return 0;
}

static int reset(void)
{
    return main_menu_reset_settings() ? MENU_ATTACHED_USB : 0;
}

static int language(void)
{
    static struct browse_folder_info langs = { LANG_DIR, SHOW_LNG };
    browse_folder(&langs);
    return 0;
}

/* A checklist of the menu-mask bits with an optional independent row. */
struct checklist {
    const struct check_row *rows;
    int count;
    int *mask;
    int defaults;
    int question;
    bool *extra;
    int extra_index;
};


static const char *checklist_name(int item, void *data, char *buf, size_t len)
{
    struct checklist *list = data;
    if (item >= list->count)
        return str(LANG_FRUIT_RESET_MENU);
    if (!list->rows[item].indent)
        return str(list->rows[item].lang);
    snprintf(buf, len, "  %s", str(list->rows[item].lang));
    return buf;
}

static const char *checklist_value(int item, void *data, char *buf,
                                   size_t len)
{
    struct checklist *list = data;
    (void)buf;
    (void)len;
    if (item >= list->count)
        return NULL;
    bool checked;
    if (list->extra && item == list->extra_index)
        checked = *list->extra;
    else
    {
        int bit = item - (list->extra && item > list->extra_index);
        checked = *list->mask & (1u << bit);
    }
    return checked ? "\xe2\x9c\x93" : NULL;
}

/* A ticked row shows its tick as a value; Reset Menu opens a question */
static enum list_row_kind checklist_kind(int item, void *data)
{
    struct checklist *list = data;
    return item < list->count ? LIST_ROW_SETTING : LIST_ROW_MENU;
}

static int checklist_action(int action, struct gui_synclist *lists)
{
    struct checklist *list = lists->data;
    if (action != ACTION_STD_OK)
        return action;
    int sel = gui_synclist_get_sel_pos(lists);
    if (list->extra && sel == list->extra_index)
        *list->extra = !*list->extra;
    else if (sel < list->count)
    {
        int bit = sel - (list->extra && sel > list->extra_index);
        *list->mask ^= 1u << bit;
    }
    else
    {
        const char *lines[] = { ID2P(list->question),
                                ID2P(LANG_FRUIT_RESET_MENU_DETAIL) };
        const struct text_message message = { lines, 2 };
        if (gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES)
        {
            *list->mask = list->defaults;
            if (list->extra)
                *list->extra = find_setting(list->extra)->default_val.bool_;
        }
    }
    settings_save();
    return ACTION_REDRAW;
}

static int checklist_show(int title, struct checklist *list)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(title), list->count + 1, list);
    info.get_name = checklist_name;
    info.get_value = checklist_value;
    info.get_kind = checklist_kind;
    info.action_callback = checklist_action;
    /* Menu goes back to Settings, as everywhere in the stock tree */
    bool previous = action_set_menu_back(true);
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "stock-checklist-push");
    bool usb = simplelist_show_list(&info);
    ui_scene_close_modal(&scene, !usb, "stock-checklist-pop");
    action_set_menu_back(previous);
    return 0;
}

static int main_menu_rows(void);

static int music_menu_rows(void)
{
    struct checklist list = {
        music_checks, MUSIC_ROWS, &global_settings.fruit_music_menu,
        find_setting(&global_settings.fruit_music_menu)->default_val.int_,
        LANG_FRUIT_RESET_MUSIC_MENU, NULL, 0 };
    return checklist_show(LANG_FRUIT_MUSIC_MENU, &list);
}

/* Stock's names for settings AstraPod's own rows name otherwise: each the
 * same setting as AstraPod's row for it. */
MENUITEM_SETTING_W_LABEL(shuffle_row, &global_settings.shuffle_mode,
                         ID2P(LANG_SHUFFLE), NULL);
MENUITEM_SETTING_W_LABEL(repeat_row, &global_settings.repeat_mode,
                         ID2P(LANG_REPEAT), NULL);
MENUITEM_SETTING_W_LABEL(clicker_row, &global_settings.keyclick_hardware,
                         ID2P(LANG_FRUIT_CLICKER), NULL);
MENUITEM_SETTING_W_LABEL(sound_check_row,
                         &global_settings.replaygain_settings.type,
                         ID2P(LANG_FRUIT_SOUND_CHECK), NULL);
MENUITEM_SETTING_W_LABEL(clock_24_row, &global_settings.timeformat,
                         ID2P(LANG_FRUIT_24_HOUR_CLOCK), NULL);
MENUITEM_SETTING_W_LABEL(time_in_title_row, &global_settings.time_in_title,
                         ID2P(LANG_FRUIT_TIME_IN_TITLE), NULL);
MENUITEM_SETTING_W_LABEL(sort_by_row, &global_settings.contacts_sort,
                         ID2P(LANG_FRUIT_SORT_BY), NULL);

ROW(date_row, LANG_FRUIT_DATE, set_date_time, NULL);
ROW(time_row, LANG_FRUIT_TIME, set_date_time, NULL);
ROW(time_zone_row, LANG_FRUIT_TIME_ZONE, not_available, NULL);

static const struct menu_item_ex *const date_time_rows[] = {
    &date_row, &time_row, &time_zone_row, &clock_24_row, &time_in_title_row,
};

MENU(date_time_menu, LANG_FRUIT_DATE_TIME, NULL, date_time_rows);

ROW(about_row, LANG_ABOUT, show_about, NULL);
ROW(main_menu_row, LANG_MAIN_MENU, main_menu_rows, NULL);
ROW(music_menu_row, LANG_FRUIT_MUSIC_MENU, music_menu_rows, NULL);
ROW(volume_limit_row, LANG_FRUIT_VOLUME_LIMIT, volume_limit, NULL);
ROW(backlight_row, LANG_BACKLIGHT, backlight, NULL);
ROW(brightness_row, LANG_BRIGHTNESS, brightness, NULL);
ROW(audiobooks_row, LANG_AUDIOBOOKS, audiobook_speed, NULL);
ROW(eq_row, LANG_FRUIT_EQ, eq_presets, NULL);
ROW(language_row, LANG_LANGUAGE, language, NULL);
ROW(legal_row, LANG_FRUIT_LEGAL, legal, NULL);
ROW(reset_row, LANG_RESET, reset, NULL);
ROW(advanced_row, LANG_FRUIT_ADVANCED_SETTINGS, advanced_settings, NULL);

static const struct menu_item_ex *const settings_rows[] = {
    &about_row, &shuffle_row, &repeat_row, &main_menu_row, &music_menu_row,
    &volume_limit_row, &backlight_row, &brightness_row, &audiobooks_row,
    &eq_row, &sound_check_row, &clicker_row, &date_time_menu, &sort_by_row,
    &language_row, &legal_row, &reset_row, &advanced_row,
};

MENU(settings_menu, LANG_SETTINGS, NULL, settings_rows);

/* Sets a bool or choice setting to `value` and runs its callback, as its
 * picker does when the value changes */
static void set_value(const void *variable, int value)
{
    const struct settings_list *setting = find_setting(variable);
    if (value == option_value_as_int(setting))
        return;
    if ((setting->flags & F_T_MASK) == F_T_BOOL)
    {
        *(bool *)setting->setting = value;
        if (setting->bool_setting->option_callback)
            setting->bool_setting->option_callback(value);
    }
    else
    {
        *(int *)setting->setting = value;
        if (setting->choice_setting->option_callback)
            setting->choice_setting->option_callback(value);
    }
    settings_save();
}

/* Stock's in-place rows. The word `item` shows for its value, or -1 for a
 * row of any other kind; with `step`, Select's next value in stock's order
 * is set first. A value stock has no word for, set in Advanced Settings,
 * shows AstraPod's and steps to the first of stock's. */
static int in_place(const struct menu_item_ex *item, bool step)
{
    struct user_settings *g = &global_settings;
    if (item == &shuffle_row)
    {
        static const int labels[] = {
            [SHUFFLE_OFF] = LANG_OFF,
            [SHUFFLE_SONGS] = LANG_FRUIT_SONGS,
            [SHUFFLE_ALBUMS] = LANG_ALBUMS,
        };
        if (step)
            set_value(&g->shuffle_mode,
                      (g->shuffle_mode + 1) % NUM_SHUFFLE_MODES);
        return labels[g->shuffle_mode];
    }
    if (item == &repeat_row)
    {
        if (step)
            set_value(&g->repeat_mode, g->repeat_mode == REPEAT_OFF ?
                      REPEAT_ONE : g->repeat_mode == REPEAT_ONE ?
                      REPEAT_ALL : REPEAT_OFF);
        switch (g->repeat_mode)
        {
            case REPEAT_OFF: return LANG_OFF;
            case REPEAT_ONE: return LANG_REPEAT_ONE;
            case REPEAT_ALL: return LANG_ALL;
            case REPEAT_SHUFFLE: return LANG_SHUFFLE;
            default: return LANG_REPEAT_AB;
        }
    }
    if (item == &sound_check_row)
    {
        /* On is the setting's default, track gain in shuffle and album
         * gain otherwise; Track or Album gain also show as On */
        int *type = &g->replaygain_settings.type;
        if (step)
        {
            set_value(type, *type == REPLAYGAIN_OFF ? REPLAYGAIN_SHUFFLE :
                      REPLAYGAIN_OFF);
            replaygain_update();
        }
        return *type == REPLAYGAIN_OFF ? LANG_OFF : LANG_ON;
    }
    if (item == &clicker_row)
    {
        if (step)
            set_value(&g->keyclick_hardware, !g->keyclick_hardware);
        return g->keyclick_hardware ? LANG_ON : LANG_OFF;
    }
    if (item == &clock_24_row)
    {
        /* timeformat 0 is the 24-hour clock */
        if (step)
            set_value(&g->timeformat, !g->timeformat);
        return g->timeformat == 0 ? LANG_ON : LANG_OFF;
    }
    if (item == &time_in_title_row)
    {
        if (step)
            set_value(&g->time_in_title, !g->time_in_title);
        return g->time_in_title ? LANG_ON : LANG_OFF;
    }
    if (item == &sort_by_row)
    {
        if (step)
            set_value(&g->contacts_sort, !g->contacts_sort);
        return g->contacts_sort ? LANG_CONTACTS_LAST : LANG_CONTACTS_FIRST;
    }
    return -1;
}

bool fruit_menu_describe(const struct menu_item_ex *item,
                         enum list_row_kind *kind, const char **value)
{
    int word = in_place(item, false);
    if (word < 0)
        return false;
    *kind = LIST_ROW_SETTING;
    *value = str(word);
    return true;
}

bool fruit_menu_step(const struct menu_item_ex *item)
{
    return in_place(item, true) >= 0;
}

/** The main menu **/

enum {
    HOME_MUSIC, HOME_COVER_FLOW, HOME_PLAYLISTS, HOME_ARTISTS, HOME_ALBUMS,
    HOME_COMPILATIONS, HOME_SONGS, HOME_RADIO, HOME_GENRES, HOME_COMPOSERS,
    HOME_AUDIOBOOKS, HOME_SEARCH, HOME_PHOTOS, HOME_PODCASTS, HOME_VIDEOS,
    HOME_VIDEO_PLAYLISTS, HOME_MOVIES, HOME_TV_SHOWS, HOME_MUSIC_VIDEOS,
    HOME_RENTALS, HOME_EXTRAS, HOME_CLOCKS, HOME_ALARMS, HOME_CALENDARS,
    HOME_CONTACTS, HOME_GAMES, HOME_NOTES, HOME_SCREEN_LOCK, HOME_STOPWATCH,
    HOME_SHUFFLE_SONGS, HOME_SLEEP, HOME_PLAYBACK_REPORT, HOME_ROWS
};

/* Settings > Main Menu, as stock lists it: a row's sub-items indented */
static const struct check_row home_checks[HOME_ROWS + 1] = {
    { LANG_MUSIC_BROWSER, false }, { LANG_COVER_FLOW_TITLE, true },
    { LANG_PLAYLISTS, true }, { LANG_ARTISTS, true }, { LANG_ALBUMS, true },
    { LANG_FRUIT_COMPILATIONS, true }, { LANG_FRUIT_SONGS, true },
    { LANG_FRUIT_RADIO, true }, { LANG_FRUIT_GENRES, true },
    { LANG_FRUIT_COMPOSERS, true }, { LANG_AUDIOBOOKS, true },
    { LANG_SEARCH, true }, { LANG_FRUIT_PHOTOS, false },
    { LANG_PODCASTS, false }, { LANG_FRUIT_ITUNES_U, false },
    { LANG_FRUIT_VIDEOS, false },
    { LANG_FRUIT_VIDEO_PLAYLISTS, true }, { LANG_FRUIT_MOVIES, true },
    { LANG_FRUIT_TV_SHOWS, true }, { LANG_FRUIT_MUSIC_VIDEOS, true },
    { LANG_FRUIT_RENTALS, true }, { LANG_FRUIT_EXTRAS, false },
    { LANG_FRUIT_CLOCKS, true }, { LANG_FRUIT_ALARMS, true },
    { LANG_FRUIT_CALENDARS, true }, { LANG_FRUIT_CONTACTS, true },
    { LANG_PLUGIN_GAMES, true }, { LANG_FRUIT_NOTES, true },
    { LANG_FRUIT_SCREEN_LOCK, true }, { LANG_FRUIT_STOPWATCH, true },
    { LANG_SHUFFLE_SONGS, false }, { LANG_FRUIT_SLEEP, false },
    { LANG_SPUN, false },
};

static int main_menu_rows(void)
{
    struct checklist list = {
        home_checks, ARRAYLEN(home_checks), &global_settings.fruit_main_menu,
        find_setting(&global_settings.fruit_main_menu)->default_val.int_,
        LANG_FRUIT_RESET_MAIN_MENU, &global_settings.fruit_home_itunes_u,
        HOME_VIDEOS };
    return checklist_show(LANG_MAIN_MENU, &list);
}

/* The main menu's own copies of rows that Music, Videos and Extras also
 * have: a row's callback, which hides it, is the item's own. */
ROW(home_cover_flow, LANG_COVER_FLOW_TITLE, cover_flow, home_callback);
ROW(home_playlists, LANG_PLAYLISTS, playlists, home_callback);
ROW(home_artists, LANG_ARTISTS, artists, home_callback);
ROW(home_albums, LANG_ALBUMS, albums, home_callback);
ROW(home_compilations, LANG_FRUIT_COMPILATIONS, compilations, home_callback);
ROW(home_songs, LANG_FRUIT_SONGS, songs, home_callback);
ROW(home_genres, LANG_FRUIT_GENRES, genres, home_callback);
ROW(home_composers, LANG_FRUIT_COMPOSERS, composers, home_callback);
ROW(home_audiobooks, LANG_AUDIOBOOKS, audiobooks, home_callback);
ROW(home_search, LANG_SEARCH, search, home_callback);
ROW(home_movies, LANG_FRUIT_MOVIES, movies, home_callback);
ROW(home_rentals, LANG_FRUIT_RENTALS, empty, home_callback);
ROW(home_tv_shows, LANG_FRUIT_TV_SHOWS, empty, home_callback);
ROW(home_music_videos, LANG_FRUIT_MUSIC_VIDEOS, empty, home_callback);
ROW(home_video_playlists, LANG_FRUIT_VIDEO_PLAYLISTS, empty, home_callback);
ROW(home_podcasts, LANG_PODCASTS, podcasts, home_callback);
ROW(home_itunes_u, LANG_FRUIT_ITUNES_U, itunes_u, home_callback);
ROW(home_clocks, LANG_FRUIT_CLOCKS, clocks, home_callback);
ROW(home_calendars, LANG_FRUIT_CALENDARS, calendars, home_callback);
ROW(home_contacts, LANG_FRUIT_CONTACTS, contacts, home_callback);
ROW(home_alarms, LANG_FRUIT_ALARMS, alarms, home_callback);
ROW(home_games, LANG_PLUGIN_GAMES, games, home_callback);
ROW(home_notes, LANG_FRUIT_NOTES, notes, home_callback);
ROW(home_screen_lock, LANG_FRUIT_SCREEN_LOCK, screen_lock, home_callback);
ROW(home_stopwatch, LANG_FRUIT_STOPWATCH, stopwatch, home_callback);
ROW(home_shuffle, LANG_SHUFFLE_SONGS, shuffle_songs, home_callback);
ROW(home_sleep, LANG_FRUIT_SLEEP, sleep_timer, home_callback);
ROW(home_playback_report, LANG_SPUN, playback_report, home_callback);
ROW(home_now_playing, LANG_NOW_PLAYING, now_playing, home_callback);

static const struct row_bit home_bits[] = {
    { &music_menu_fruit, HOME_MUSIC }, { &home_cover_flow, HOME_COVER_FLOW },
    { &home_playlists, HOME_PLAYLISTS }, { &home_artists, HOME_ARTISTS },
    { &home_albums, HOME_ALBUMS }, { &home_compilations, HOME_COMPILATIONS },
    { &home_songs, HOME_SONGS }, { &home_genres, HOME_GENRES },
    { &home_composers, HOME_COMPOSERS }, { &home_audiobooks, HOME_AUDIOBOOKS },
    { &home_search, HOME_SEARCH }, { &photos_menu, HOME_PHOTOS },
    { &home_podcasts, HOME_PODCASTS }, { &videos_menu, HOME_VIDEOS },
    { &home_video_playlists, HOME_VIDEO_PLAYLISTS },
    { &home_movies, HOME_MOVIES }, { &home_tv_shows, HOME_TV_SHOWS },
    { &home_music_videos, HOME_MUSIC_VIDEOS }, { &home_rentals, HOME_RENTALS },
    { &extras_menu, HOME_EXTRAS }, { &home_clocks, HOME_CLOCKS },
    { &home_alarms, HOME_ALARMS }, { &home_calendars, HOME_CALENDARS },
    { &home_contacts, HOME_CONTACTS }, { &home_games, HOME_GAMES },
    { &home_notes, HOME_NOTES }, { &home_screen_lock, HOME_SCREEN_LOCK },
    { &home_stopwatch, HOME_STOPWATCH }, { &home_shuffle, HOME_SHUFFLE_SONGS },
    { &home_sleep, HOME_SLEEP },
    { &home_playback_report, HOME_PLAYBACK_REPORT },
};

/* Hides the main menu's rows Settings > Main Menu turned off, and Now
 * Playing while nothing plays. Music, Videos, Photos and Extras carry it
 * too, for the main menu that lists them. */
static int home_callback(int action, const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    if (this_item == &home_itunes_u)
        return global_settings.fruit_home_itunes_u &&
               browser_db_fruit_count(FRUIT_BROWSE_ITUNES_U) > 0 ?
               action : ACTION_EXIT_MENUITEM;
    if (this_item == &home_now_playing)
        return audio_status() ? action : ACTION_EXIT_MENUITEM;
    int bit = bit_of(home_bits, ARRAYLEN(home_bits), this_item);
    if (bit >= 0 && !(global_settings.fruit_main_menu & (1u << bit)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

/* The main menu in the order it shows its rows: stock's seven, each
 * followed by the sub-items Settings > Main Menu put beside it, then
 * AstraPod's Playback Report, which starts hidden */
static const struct menu_item_ex *const home_rows[] = {
    &music_menu_fruit, &home_cover_flow, &home_playlists, &home_artists,
    &home_albums, &home_compilations, &home_songs, &home_genres,
    &home_composers, &home_audiobooks, &home_search,
    &videos_menu, &home_video_playlists, &home_movies, &home_tv_shows,
    &home_music_videos, &home_rentals,
    &photos_menu, &home_podcasts, &home_itunes_u,
    &extras_menu, &home_clocks, &home_alarms, &home_calendars,
    &home_contacts, &home_games, &home_notes, &home_screen_lock,
    &home_stopwatch,
    &settings_menu, &home_shuffle, &home_sleep, &home_playback_report,
    &home_now_playing,
};

MENU(home_menu, LANG_ROCKBOX_TITLE, NULL, home_rows);

const struct menu_item_ex *fruit_menu_root(void)
{
    return &home_menu;
}

/** The pane **/

bool fruit_menu_owns(const struct menu_item_ex *menu)
{
    return menu == &home_menu || menu == &music_menu_fruit ||
           menu == &videos_menu || menu == &photos_menu ||
           menu == &extras_menu;
}

bool fruit_menu_is_stock(const struct menu_item_ex *menu)
{
    return fruit_menu_owns(menu) || menu == &settings_menu ||
           menu == &date_time_menu;
}

/* Beside a stock Extras item, stock's picture of it at stock's place, and
 * the caption under it. Whether `item` is one. */
static bool extras_pane(const struct menu_item_ex *item)
{
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = -1, .baseline = 221,
    };
    /* Stock shows the Clocks picture beside Extras itself too */
    if (item == &extras_clocks_row || item == &home_clocks ||
        item == &extras_menu)
    {
        struct tm *tm = get_time();
        c.paint = extras_clock_pane_paint;
        c.stamp = tm->tm_hour * 60 + tm->tm_min;
        snprintf(c.title, sizeof(c.title), "%s %d %d",
                 str(LANG_MONTH_JANUARY + tm->tm_mon), tm->tm_mday,
                 tm->tm_year + 1900);
    }
    else if (item == &extras_calendars_row || item == &home_calendars)
        extras_calendars_pane(&c);
    else if (item == &extras_contacts_row || item == &home_contacts)
    {
        c.art = STOCK_ART_ADDRESSVIEWER_PREVIEW;
        c.x = 186, c.y = 47;
        strlcpy(c.title, str(extras_contacts_any() ? LANG_FRUIT_CONTACTS :
                             LANG_FRUIT_NO_CONTACTS), sizeof(c.title));
    }
    else if (item == &extras_alarms_row || item == &home_alarms)
        c.art = STOCK_ART_ALARMS_CLOCK_STATIC, c.x = 178, c.y = 56;
    else if (item == &extras_notes_row || item == &home_notes)
    {
        c.art = STOCK_ART_NOTES_PREVIEW;
        c.x = 180, c.y = 47;
        strlcpy(c.title, str(LANG_FRUIT_NOTES), sizeof(c.title));
    }
    else if (item == &extras_screen_lock_row || item == &home_screen_lock)
    {
        c.art = STOCK_ART_LOCKSCREEN_PADLOCK;
        c.x = 179, c.y = 22;
        strlcpy(c.title, str(extras_screen_lock_set() ?
                             LANG_EXTRAS_SCREEN_LOCK_SET :
                             LANG_EXTRAS_SCREEN_LOCK_NONE), sizeof(c.title));
    }
    else if (item == &extras_stopwatch_row || item == &home_stopwatch)
        c.paint = extras_stopwatch_pane_paint;
    else if (item == &extras_games_row || item == &home_games)
    {
        int games = app_list_count(APP_LIST_GAMES);
        c.art = STOCK_ART_EXTRAS_GAMES_PREVIEW;
        c.x = 170, c.y = 83;
        snprintf(c.title, sizeof(c.title), str(games == 1 ?
                 LANG_FRUIT_ONE_GAME : LANG_FRUIT_GAMES_COUNT), games);
    }
    else
        return false;
    menu_pane_set_picture(&c);
    return true;
}

static bool fruit_media_pane(const struct menu_item_ex *item)
{
    static const struct {
        const struct menu_item_ex *music, *home;
        enum fruit_browse_kind kind;
        int art, empty, one, many;
    } rows[] = {
        { &music_compilations, &home_compilations, FRUIT_BROWSE_COMPILATIONS,
          STOCK_ART_NO_CONTENT_MUSIC_PREVIEW, LANG_FRUIT_NO_COMPILATIONS, 0, 0 },
        { &music_composers, &home_composers, FRUIT_BROWSE_COMPOSERS,
          STOCK_ART_NO_CONTENT_MUSIC_PREVIEW, LANG_FRUIT_NO_COMPOSERS, 0, 0 },
        { &music_audiobooks, &home_audiobooks, FRUIT_BROWSE_AUDIOBOOKS,
          STOCK_ART_NO_CONTENT_AUDIOBOOKS_PREVIEW, LANG_FRUIT_NO_AUDIOBOOKS,
          LANG_FRUIT_ONE_AUDIOBOOK, LANG_FRUIT_AUDIOBOOK_COUNT },
        { NULL, &home_podcasts, FRUIT_BROWSE_PODCASTS,
          STOCK_ART_NO_CONTENT_PODCAST_PREVIEW, LANG_FRUIT_NO_PODCASTS,
          LANG_FRUIT_ONE_PODCAST, LANG_FRUIT_PODCAST_COUNT },
        { NULL, &home_itunes_u, FRUIT_BROWSE_ITUNES_U,
          STOCK_ART_NO_CONTENT_PODCAST_PREVIEW, LANG_FRUIT_ITUNES_U, 0, 0 },
    };
    for (unsigned i = 0; i < ARRAYLEN(rows); i++)
    {
        if (item != rows[i].home && item != rows[i].music)
            continue;
        int count = browser_db_fruit_count(rows[i].kind);
        if (count != 0 && !rows[i].many && rows[i].kind != FRUIT_BROWSE_ITUNES_U)
            return false;

        struct menu_pane_content c = {
            .art = rows[i].art, .x = 180, .y = 44, .baseline = 163,
        };
        if (count > 0 && rows[i].many)
            snprintf(c.title, sizeof(c.title),
                     str(count == 1 ? rows[i].one : rows[i].many), count);
        else
            strlcpy(c.title, str(rows[i].empty), sizeof(c.title));
        menu_pane_set_picture(&c);
        return true;
    }
    return false;
}

enum list_pane_kind fruit_menu_item_pane(const struct menu_item_ex *menu,
                                         const struct menu_item_ex *item,
                                         int *art)
{
    if (extras_pane(item) || fruit_media_pane(item))
        return LIST_PANE_PICTURE;
    int bit = bit_of(home_bits, ARRAYLEN(home_bits), item);
    if (bit < 0)
        bit = bit_of(music_bits, ARRAYLEN(music_bits), item) >= 0 ?
              HOME_COVER_FLOW : -1;
    if (menu == &videos_menu || (bit >= HOME_VIDEOS && bit <= HOME_RENTALS))
        bit = HOME_VIDEOS;
    if (menu == &photos_menu)
        bit = HOME_PHOTOS;
    if (menu == &extras_menu)
        bit = HOME_GAMES;

    *art = STOCK_ART_NO_CONTENT_MUSIC_PREVIEW;
    if (item == &music_audiobooks || item == &home_audiobooks)
        *art = STOCK_ART_NO_CONTENT_AUDIOBOOKS_PREVIEW;
    else if (item == &music_playlists || item == &home_playlists)
        *art = STOCK_ART_NO_CONTENT_PLAYLIST_PREVIEW;
    else if (bit >= HOME_MUSIC && bit <= HOME_SEARCH)
        return LIST_PANE_ART;
    else if (item == &settings_menu)
        return LIST_PANE_DEVICE;
    else if (item == &home_shuffle)
        return LIST_PANE_SHUFFLE;
    else if (item == &home_now_playing)
        return LIST_PANE_PLAYING;
    else if (bit == HOME_VIDEOS)
        *art = STOCK_ART_NO_CONTENT_MOVIES_PREVIEW;
    else if (bit == HOME_PHOTOS)
        *art = STOCK_ART_NO_CONTENT_PHOTO_PREVIEW;
    else if (bit == HOME_PODCASTS)
        *art = STOCK_ART_NO_CONTENT_PODCAST_PREVIEW;
    else
        *art = STOCK_ART_EXTRAS_GAMES_PREVIEW;
    return LIST_PANE_CATEGORY;
}
