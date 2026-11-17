/* Cider-bound native Now Playing menus. GPL version 2 or later. */
#include "config.h"
#include "misc.h"
#include "settings/settings.h"
#include "cider.h"
#include "cider_controls.h"
#include "cider_library.h"
#include "cider_wps.h"
#include "input/action.h"
#include "widgets/menu.h"
#include "widgets/splash.h"
#include "widgets/text_box.h"
#include "screens/settings/exported_settings.h"
#include "system/activity.h"
#include "system/app_util.h"
#include "lang.h"
#include "lcd.h"
#include "kernel.h"
#include "usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cider_mode_title(int mode)
{
    return str(mode == CIDER_MODE_REPEAT ? LANG_REPEAT : LANG_SHUFFLE);
}
const char *cider_mode_label(int mode, int value)
{
    if (mode == CIDER_MODE_REPEAT)
        return str(value == 1 ? LANG_ALL : value == 2 ? LANG_REPEAT_ONE :
                   value == 3 ? LANG_SHUFFLE : value == 4 ? LANG_REPEAT_AB : LANG_OFF);
    return str(value ? LANG_ON : LANG_OFF);
}

/* No large automatic objects on the 8 KiB UI stack. */
static struct cider_row row;
static char fields[12][124];
static bool page_usb;
static int page_mode, requested_exit;
static const char *const field_ids[] = {"title", "artist", "album", "disc", "track",
    "year", "genre", "duration", "quality", "favorite", "library", "speed"};
static const int field_labels[] = {LANG_ID3_TITLE, LANG_ID3_ARTIST, LANG_ID3_ALBUM,
    LANG_ID3_DISCNUM, LANG_ID3_TRACKNUM, LANG_ID3_YEAR, LANG_ID3_GENRE,
    LANG_ID3_LENGTH, LANG_SOUND_PROPERTIES};

static int load_page(const char *route)
{
    char query[80];
    snprintf(query, sizeof query, "%s?nonce=%lu", route, (unsigned long)current_tick);
    cider_page_open(query);
    long deadline = current_tick + 5 * HZ;
    int n;
    while ((n = cider_page_total()) < 0 && TIME_BEFORE(current_tick, deadline))
    {
        int action = get_action(CONTEXT_STD, HZ / 20);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU) return -1;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        { page_usb = true; return -1; }
    }
    if (n < 0) splashf(HZ, "%s did not answer", cider_remote_name());
    return n;
}
static bool read_info(void)
{
    memset(fields, 0, sizeof fields);
    int n = load_page("now/info");
    for (int i = 0; i < n; i++)
        if (cider_page_row(i, &row))
            for (unsigned int j = 0; j < ARRAYLEN(field_ids); j++)
                if (!strcmp(row.id, field_ids[j]))
                    snprintf(fields[j], sizeof fields[j], "%s", row.title);
    return n > 0;
}

static const char *page_name(int item, void *data, char *buf, size_t len)
{
    (void)data;
    if (!cider_page_row(item, &row)) return "...";
    if (page_mode == 1)
    {
        for (unsigned int j = 0; j < ARRAYLEN(field_labels); j++)
            if (!strcmp(row.id, field_ids[j]))
            {
                if (j == 7)
                {
                    unsigned long seconds = strtoul(row.title, NULL, 10) / 1000;
                    snprintf(buf, len, "%s: %lu:%02lu", str(field_labels[j]), seconds / 60, seconds % 60);
                }
                else snprintf(buf, len, "%s: %s", str(field_labels[j]), row.title);
                return buf;
            }
        return "";
    }
    if (page_mode == 3)
        snprintf(buf, len, "%s: %s", str(!strcmp(row.id, "tracks") ?
                 LANG_TAGNAVI_ALL_TRACKS : LANG_PLAYING_TIME), row.title);
    else snprintf(buf, len, "%s", row.title);
    return buf;
}
static int page_action(int action, struct gui_synclist *list)
{
    if (action == ACTION_STD_MENU) return ACTION_STD_CANCEL;
    if (action == ACTION_STD_OK)
    {
        int index = gui_synclist_get_sel_pos(list);
        if (page_mode == 2) cider_control("queue-jump", index);
        else if (cider_page_row(index, &row))
        {
            const char *title = str(page_mode == 0 ? LANG_LYRICS : LANG_TRACK_INFO);
            if (view_text(title, row.title))
            { page_usb = true; return ACTION_STD_CANCEL; }
        }
        return ACTION_REDRAW;
    }
    return action;
}
static int show_page(const char *route, int title, int mode)
{
    int n = load_page(route);
    if (n <= 0) return page_usb;
    page_mode = mode;
    /* now/info's first nine rows have the native Track Info fields. The
     * status/capability fields at its end are private to this menu. */
    if (mode == 1) n = MIN(n, 9);
    struct simplelist_info info;
    simplelist_info_init(&info, str(title), n, NULL);
    info.get_name = page_name;
    info.action_callback = page_action;
    if (mode == 2 && cider_page_row(0, &row)) info.selection = MAX(0, MIN(n - 1, row.count));
    return simplelist_show_list(&info) || page_usb;
}
static int mode_choose(void *data)
{
    int mode = (intptr_t)data;
    struct simplelist_info info;
    /* A native list of the exact FU26 values. A tap selects an explicit value. */
    simplelist_info_init(&info, (char *)cider_mode_title(mode), mode ? 5 : 2, data);
    info.selection = cider_mode_get(mode);
    extern const char *cider_mode_choice(int, void *, char *, size_t);
    info.get_name = cider_mode_choice;
    if (simplelist_show_list(&info)) return 1;
    if (info.selection >= 0) cider_mode_set(mode, info.selection);
    return 0;
}
const char *cider_mode_choice(int item, void *data, char *buf, size_t len)
{
    (void)buf; (void)len;
    return cider_mode_label((intptr_t)data, item);
}
static char *mode_name(int item, void *data, char *buf, size_t len)
{
    (void)item;
    int mode = (intptr_t)data;
    snprintf(buf, len, "%s", cider_mode_title(mode));
    return buf;
}
static int rating(void)
{
    if (!read_info()) return page_usb;
    const char *choices[] = {atoi(fields[9]) ? "Unlove" : "Love", "Add to Library"};
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_MENU_SET_RATING), atoi(fields[10]) ? 1 : 2, NULL);
    info.get_name = NULL;
    simplelist_reset_lines();
    for (int i = 0; i < info.count; i++) simplelist_addline("%s", choices[i]);
    if (simplelist_show_list(&info)) return 1;
    if (info.selection >= 0)
        cider_control(info.selection == 1 ? "add-library" : atoi(fields[9]) ? "unlove" : "love", 0);
    return 0;
}
static int track_info(void) { return show_page("now/info", LANG_TRACK_INFO, 1); }
static int lyrics(void) { return show_page("now/lyrics", LANG_LYRICS, 0); }
static int queue_view(void) { return show_page("now/queue", LANG_CURRENT_PLAYLIST, 2); }
static int queue_time(void) { return show_page("now/time", LANG_PLAYING_TIME, 3); }
static int browse(void *data)
{
    bool artist = (intptr_t)data;
    if (load_page(artist ? "now/artist" : "now/album") <= 0 || !cider_page_row(0, &row)) return page_usb;
    int destination = cider_browse_at(&row);
    if (destination == GO_TO_PREVIOUS) return 0;
    requested_exit = destination;
    return 1;
}
static int speed(void)
{
    static const char *const labels[] = {"0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x"};
    static const int rates[] = {500, 750, 1000, 1250, 1500, 2000};
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_SPEED), ARRAYLEN(rates), NULL);
    simplelist_reset_lines();
    for (unsigned int i = 0; i < ARRAYLEN(rates); i++) simplelist_addline("%s", labels[i]);
    info.selection = 2;
    if (simplelist_show_list(&info)) return 1;
    if (info.selection >= 0) cider_control("rate", rates[info.selection]);
    return 0;
}
static int volume_action(int action, struct gui_synclist *list)
{
    (void)list;
    if (action == ACTION_STD_MENU) return ACTION_STD_CANCEL;
    if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT || action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
    {
        cider_control("volume-step", action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT ? 1 : -1);
        return ACTION_REDRAW;
    }
    return action;
}
static const char *volume_name(int item, void *data, char *buf, size_t len)
{
    (void)item; (void)data;
    snprintf(buf, len, "%s: %d", str(LANG_VOLUME), cider_wps_volume(NULL));
    return buf;
}
static int volume(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_VOLUME), 1, NULL);
    info.get_name = volume_name; info.action_callback = volume_action;
    return simplelist_show_list(&info);
}
static int albumart(void) { return cider_wps_show_art(); }
extern int cider_native_sleep_timer(void);
extern char *cider_native_sleep_name(int, void *, char *, size_t);
MENUITEM_FUNCTION_DYNTEXT_W_PARAM(cider_shuffle, MENU_FUNC_CHECK_RETVAL, mode_choose, (void *)0, mode_name, NULL, (void *)0, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT_W_PARAM(cider_repeat, MENU_FUNC_CHECK_RETVAL, mode_choose, (void *)1, mode_name, NULL, (void *)1, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_rating_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_MENU_SET_RATING), rating, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_info_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_TRACK_INFO), track_info, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(cider_album_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_GO_TO_ALBUM), browse, (void *)0, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(cider_artist_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_GO_TO_ARTIST), browse, (void *)1, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_lyrics_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_LYRICS), lyrics, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(cider_sleep_item, MENU_FUNC_CHECK_RETVAL, cider_native_sleep_timer, cider_native_sleep_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_speed_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_SPEED), speed, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_queue_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_VIEW_DYNAMIC_PLAYLIST), queue_view, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_time_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_PLAYING_TIME), queue_time, NULL, Icon_NOICON);
MAKE_MENU(cider_queue_menu, ID2P(LANG_CURRENT_PLAYLIST), NULL, Icon_Playlist, &cider_queue_item, &cider_time_item);
MENUITEM_FUNCTION(cider_art_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_VIEW_ALBUMART), albumart, NULL, Icon_NOICON);
MENUITEM_FUNCTION(cider_volume_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_VOLUME), volume, NULL, Icon_NOICON);
MAKE_MENU(cider_sound_menu, ID2P(LANG_SOUND_SETTINGS), NULL, Icon_Audio, &cider_volume_item);
MAKE_MENU(cider_more_menu, ID2P(LANG_MORE), NULL, Icon_NOICON, &cider_art_item, &cider_sound_menu);
MAKE_MENU(cider_context_menu, ID2P(LANG_NOW_PLAYING), NULL, Icon_Audio,
    &cider_shuffle, &cider_repeat, &cider_rating_item, &cider_info_item, &cider_album_item, &cider_artist_item,
    &cider_lyrics_item, &cider_sleep_item, &cider_speed_item, &cider_queue_menu, &theme_options_item, &cider_more_menu);
static bool context_setting(const struct menu_item_ex *item, enum list_row_kind *kind, const char **value)
{
    int mode;
    if (item == &cider_shuffle) mode = CIDER_MODE_SHUFFLE;
    else if (item == &cider_repeat) mode = CIDER_MODE_REPEAT;
    else return false;
    *kind = mode == CIDER_MODE_SHUFFLE ? LIST_ROW_TOGGLE : LIST_ROW_SETTING;
    *value = cider_mode_label(mode, cider_mode_get(mode));
    return true;
}
static const struct menu_row_presenter context_presenter = {context_setting};
bool cider_context_show(void)
{
    page_usb = false; requested_exit = GO_TO_ROOT;
    action_wait_for_release();
    push_current_activity(ACTIVITY_CONTEXTMENU);
    bool previous_back = action_set_menu_back(true);
    const struct menu_row_presenter *previous = menu_set_row_presenter(&context_presenter);
    int result = do_menu(&cider_context_menu, NULL, NULL, false);
    menu_set_row_presenter(previous);
    action_set_menu_back(previous_back);
    pop_current_activity();
    return result == 1 || result == GO_TO_ROOT || page_usb;
}

int cider_context_destination(void) { return requested_exit; }

static const char *store_name(int item, void *data, char *buf, size_t len)
{
    (void)data;
    struct cider_store_stats stats;
    cider_store_stats(&stats);
    static const char *const states[] = {"Checking inventory", "Incomplete", "Complete", "Cannot complete: art/capacity"};
    switch (item)
    {
        case 0: return stats.build_on ? "Stop full cache build" : "Build full cover cache";
        case 1: snprintf(buf, len, "%s %lu/%lu", states[MIN(stats.build_state, 3)], (unsigned long)stats.build_done, (unsigned long)stats.build_total); break;
        case 2: snprintf(buf, len, "Loaded %lu, entries %lu", (unsigned long)stats.loaded, (unsigned long)stats.entries); break;
        case 3: snprintf(buf, len, "Bloom hits %lu / false %lu", (unsigned long)stats.bloom_hits, (unsigned long)stats.false_positives); break;
        case 4: snprintf(buf, len, "Sector reads %lu", (unsigned long)stats.sectors); break;
        case 5: snprintf(buf, len, "Hits %lu / misses %lu", (unsigned long)stats.hits, (unsigned long)stats.misses); break;
        case 6: snprintf(buf, len, "Writes %lu / idle deferrals %lu", (unsigned long)stats.writes, (unsigned long)stats.deferrals); break;
        case 7: snprintf(buf, len, "Loads %lu / last error %d", (unsigned long)stats.loads, stats.last_error); break;
        case 8: snprintf(buf, len, "Read us %lu / max %lu", (unsigned long)stats.read_us, (unsigned long)stats.max_read_us); break;
        default: snprintf(buf, len, "Read-ahead %u M covers at 61/s", cider_read_ahead_count(61, 9)); break;
    }
    return buf;
}
static int store_action(int action, struct gui_synclist *list)
{
    if (action == ACTION_STD_MENU) return ACTION_STD_CANCEL;
    if (action == ACTION_STD_OK && gui_synclist_get_sel_pos(list) == 0)
        cider_store_build(!cider_store_building());
    return action == ACTION_STD_OK || action == ACTION_NONE ? ACTION_REDRAW : action;
}
bool cider_store_debug_show(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, "Cider cover store", 10, NULL);
    info.get_name = store_name; info.action_callback = store_action; info.timeout = HZ / 5;
    return simplelist_show_list(&info);
}
