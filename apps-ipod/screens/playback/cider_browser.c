/* Cider Remote through the native themed database list and search dialog.
 * Copyright (C) 2026. GNU General Public License version 2 or later. */
#include "crumbs.h"
#include "config.h"
#include "audio.h"
#include "cider.h"
#include "cider_library.h"
#include "cider_controls.h"
#include "quick_screen.h"
#include "screens/covers/carousel.h"
#include "input/action.h"
#include "input/agents_shortcut.h"
#include "widgets/list.h"
#include "widgets/search_dialog.h"
#include "widgets/keyboard.h"
#include "widgets/splash.h"
#include "rbunicode.h"
#include "screens/browse/browser.h"
#include "screens/browse/browser_db.h"
#include "system/activity.h"
#include "settings/settings.h"
#include "ui/scene.h"
#include "core_alloc.h"
#include "draw/bmp.h"
#include "speech/talk.h"
#include "root_menu.h"
#include "lang.h"
#include "kernel.h"
#include "misc.h"
#include "usb.h"
#include "string-extra.h"
#include "metadata/art_cache.h"
#include "metadata/art_name.h"
#include <stdio.h>
#include <string.h>
static struct
{
    char route[380], title[124];
    int selected;
    struct browser_db_header header;
    char art[65];
} history[12];
static int depth;
static bool show_covers, remote_return, cover_suspended;
static int cover_parent_depth = -1;
static struct cider_row selected_row;
enum { CIDER_ROOT_FIXED_ROWS = 3, CIDER_ROOT_LIBRARY_ROWS = 29 };
static int root_count, browse_floor;
static int root_ordinals[CIDER_ROOT_LIBRARY_ROWS];
static bool empty_list;
static const char *list_notice;

enum { CIDER_LIST_ART_DIM = 56 };
struct cider_browser_view {
    fb_data pixels[CIDER_LIST_ART_DIM * CIDER_LIST_ART_DIM];
    struct bitmap cover;
    struct list_header header;
    char album_detail[80];
};

static struct cider_row callback_row;
static char query[SEARCH_MAX_QUERY + 1];
static int search_count = -1;
static int ahead_previous, ahead_direction = 1;

static void escape(char *out, unsigned int size, const char *s)
{
    unsigned int n = 0;
    for (; *s && n + 4 < size; s++)
    {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            out[n++] = c;
        else
        {
            snprintf(out + n, size - n, "%%%02X", c);
            n += 3;
        }
    }
    out[n] = 0;
}
static void root(void)
{
    int library_rows = 0;
    int total = browser_db_root_row_count();

    for (int n = 0; n < total && library_rows < CIDER_ROOT_LIBRARY_ROWS; ++n)
    {
        int index;
        if (!browser_db_get_root_row(n, &index, NULL))
            break;
        if (!browser_db_root_row_is_hidden(index))
            root_ordinals[library_rows++] = n;
    }
    root_count = CIDER_ROOT_FIXED_ROWS + library_rows + 1;
}

static bool get_root_row(int item, struct cider_row *out)
{
    static const int labels[] = {
        LANG_ALBUM_COVERS, LANG_PLAYLISTS, LANG_RESUME_PLAYBACK,
    };
    static const char *const routes[] = { "covers", "playlists", "wps" };

    memset(out, 0, sizeof(*out));
    if (item < CIDER_ROOT_FIXED_ROWS)
    {
        int title = item == 2 && (cider_active() || audio_status()) ?
                    LANG_NOW_PLAYING : labels[item];
        strlcpy(out->title, str(title), sizeof(out->title));
        strlcpy(out->route, routes[item], sizeof(out->route));
        return true;
    }
    if (item == root_count - 1)
    {
        strlcpy(out->title, str(LANG_ART_CACHE_MENU), sizeof(out->title));
        strlcpy(out->route, "cache", sizeof(out->route));
        return true;
    }

    int index;
    const unsigned char *name;
    int ordinal = root_ordinals[item - CIDER_ROOT_FIXED_ROWS];
    if (!browser_db_get_root_row(ordinal, &index, &name))
        return false;

    strlcpy(out->title, P2STR(name), sizeof(out->title));
    strlcpy(out->route, "root/", sizeof(out->route));
    escape(out->route + 5, sizeof(out->route) - 5, browser_db_root_row_key(index));
    return true;
}

static bool get_row(int i, struct cider_row *out)
{
    if (list_notice)
        return false;
    if (!depth)
        return cider_available() && i >= 0 && i < root_count && get_root_row(i, out);
    return cider_page_row(i, out);
}

static const char *name(int i, void *data, char *buf, size_t size)
{
    (void)data;
    if (list_notice)
    {
        if (!*list_notice)
            return (const char *)str(LANG_NO_FILES);
        snprintf(buf, size, list_notice, cider_remote_name());
        return buf;
    }
    if (!get_row(i, &callback_row))
        return "...";
    snprintf(buf, size, "%s", callback_row.title);
    return buf;
}
static enum themable_icons icon(int i, void *data)
{
    (void)data;
    if (!get_row(i, &callback_row))
        return Icon_NOICON;
    if (!strcmp(callback_row.route, "wps"))
        return Icon_Playback_menu;
    return callback_row.kind == CIDER_ROW_SONG       ? Icon_Audio
           : callback_row.kind == CIDER_ROW_PLAYLIST ? Icon_Playlist
                                                     : Icon_Folder;
}
static enum list_row_kind kind(int i, void *data)
{
    (void)data;
    if (!get_row(i, &callback_row))
        return LIST_ROW_PLAIN;
    return callback_row.kind == CIDER_ROW_SONG ? LIST_ROW_TRACK
                                               : LIST_ROW_CONTAINER;
}
static const struct bitmap *bitmap_for(struct cider_browser_view *view,
                                       const char *key, struct dim *size)
{
    int dim = MIN(CIDER_LIST_ART_DIM, MIN(size->width, size->height));
    if (!view || dim <= 0)
        return NULL;
    int src = CIDER_LIST_ART_DIM;
    fb_data *pixels = view->pixels;
    if (!cider_copy_art(key, "S", pixels, sizeof(view->pixels)))
    {
        cider_prefetch_art(key, "S");
        src = 32;
        if (!cider_copy_art(key, "XS", pixels, src * src * sizeof(*pixels)))
            return NULL;
    }
    /* Walk toward the end when shrinking and toward the start when expanding.
     * In either direction, an index can overwrite only an already read pixel. */
    int step = dim <= src ? 1 : -1;
    int first = step > 0 ? 0 : dim - 1;
    int end = step > 0 ? dim : -1;
    for (int y = first; y != end; y += step)
        for (int x = first; x != end; x += step)
            pixels[y * dim + x] = pixels[y * src / dim * src + x * src / dim];

    view->cover.width = view->cover.height = dim;
    view->cover.format = FORMAT_NATIVE;
    view->cover.data = (unsigned char *)pixels;
    return &view->cover;
}

static const struct bitmap *art(int i, void *data, struct dim *size)
{
    if (!get_row(i, &callback_row) || callback_row.kind != CIDER_ROW_ALBUM)
        return NULL;
    return bitmap_for(data, callback_row.art, size);
}

static const struct bitmap *artist_art_at(int item, int dim, bool paint)
{
    if (!get_row(item, &callback_row) || callback_row.kind != CIDER_ROW_ARTIST ||
        strlen(callback_row.art) < 2)
        return NULL;
    struct artist_tile_source source = {
        .identity = art_name_hash(callback_row.title) ^ art_name_hash(cider_remote_name()),
    };
    strlcpy(source.remote, callback_row.art, sizeof(source.remote));
    return artist_tile_request(&source, dim, paint);
}

static const struct bitmap *artist_art(int item, void *data, struct dim *size)
{
    (void)data;
    return artist_art_at(item, MIN(size->width, size->height), true);
}

static void artist_prefetch(int item, void *data, int dim)
{
    (void)data;
    artist_art_at(item, dim, false);
}

static const struct bitmap *header_art(void *data, struct dim *size)
{
    const char *key = history[depth].art;
    if (!*key)
        return NULL;

    const struct bitmap *artwork = bitmap_for(data, key, size);
    if (!artwork)
        cider_request_xs(&key, 1);
    return artwork;
}

static char search_route[380];
static int search_index;
static int search_scan(const char *text, void *ctx)
{
    (void)ctx;
    char value[260], route[380];
    escape(value, sizeof value, text);
    snprintf(route, sizeof route, "search?q=%s", value);
    snprintf(search_route, sizeof search_route, "%s", route);
    cider_page_open(route);
    int n = cider_page_total();
    return n < 0 ? 0 : MIN(n, 50);
}
static bool search_ready(void *ctx)
{
    (void)ctx;
    int count = cider_page_total();
    if (count == search_count)
        return false;
    search_count = count;
    return count >= 0;
}
static const char *search_text(int i, void *ctx)
{
    (void)ctx;
    if (cider_page_row(i, &callback_row))
        return callback_row.title;
    return "...";
}
static bool search(struct cider_row *out)
{
    const struct search_provider provider = {
        .title = "Search",
        .activity = ACTIVITY_DB_SEARCH,
        .scan = search_scan,
        .row_text = search_text,
        .refresh = search_ready,
    };
    int result = search_dialog_run(&provider, NULL, query, sizeof query);
    search_index = result;
    return result >= 0 && cider_page_row(result, out);
}
static void enter(const struct cider_row *r)
{
    if (depth + 1 >= (int)ARRAYLEN(history))
        return;
    depth++;
    snprintf(history[depth].route, sizeof history[depth].route, "%s", r->route);
    snprintf(history[depth].title, sizeof history[depth].title, "%s", r->title);
    history[depth].selected = 0;
    history[depth].art[0] = 0;
    memset(&history[depth].header, 0, sizeof history[depth].header);
    if (r->kind == CIDER_ROW_ALBUM)
    {
        snprintf(history[depth].header.name, sizeof history[depth].header.name,
                 "%s", r->title);
        snprintf(history[depth].header.artist,
                 sizeof history[depth].header.artist, "%s", r->subtitle);
        history[depth].header.tracks = r->count;
        history[depth].header.year = r->year;
        history[depth].header.length_ms = r->duration;
        snprintf(history[depth].art, sizeof history[depth].art, "%s", r->art);
    }
    cider_cancel_art();
    if (r->kind == CIDER_ROW_ALBUM)
        cider_pin_art(r->art);
    cider_page_open(history[depth].route);
}
static void discard_suspended_covers(void)
{
    if (!cover_suspended) return;
    cover_suspended = false;
    cover_parent_depth = -1;
    depth = 0;
    history[0].selected = 0;
}

int cider_browse_at(const struct cider_row *row)
{
    discard_suspended_covers();
    depth = 0;
    enter(row);
    browse_floor = action_menu_back() ? depth : 0;
    int result = cider_browse();
    browse_floor = 0;
    return result;
}
int cider_browse_covers(bool home)
{
    if (home || !cover_nav_playing()) discard_suspended_covers();
    if (!cover_nav_active()) cover_nav_begin(home ? GO_TO_WPS : GO_TO_PREVIOUS);
    show_covers = true;
    return GO_TO_CIDER;
}
/* One visit owns its album depth. A normal back returns to the browser level
 * that opened it; a toggle leaves the visit, never a synthetic covers node. */
static void close_covers(bool restore_parent)
{
    if (restore_parent && cover_parent_depth >= 0) depth = cover_parent_depth;
    cover_parent_depth = -1;
    show_covers = false;
    if (cover_nav_active()) cover_nav_finish();
}
static int toggle_covers(void)
{
    int destination = cover_nav_origin_is(GO_TO_AGENTS) ? GO_TO_AGENTS : GO_TO_WPS;
    agents_nav_take(AGENTS_NAV_BACK);
    close_covers(true);
    remote_return = true; /* Both native and remote gestures use this result. */
    return destination;
}

bool cider_browse_remote_return(void)
{
    bool remote = remote_return;
    remote_return = false;
    return remote;
}

static void animate_depth(struct ui_scene *scene, int previous)
{
    if (previous == depth)
        return;
    if (!scene->handle)
        ui_scene_interrupt();
    ui_scene_capture(scene, &history[previous], &history[depth], depth > previous);
}

int cider_browse(void)
{
    if (!agents_nav_enter(AGENTS_NAV_TRACKS))
        return GO_TO_PREVIOUS;
    if (!show_covers) discard_suspended_covers();
    if (!show_covers && cover_parent_depth < 0 && cover_nav_active())
        cover_nav_finish();
    if (show_covers)
        cover_nav_set_playing(false);
    cover_suspended = false;
#ifdef HAVE_CRUMBS
    crumb(CR_CIDER_IN, 0);
#endif
    if (show_covers)
        push_activity_without_refresh(ACTIVITY_DATABASEBROWSER);
    else
        push_current_activity(ACTIVITY_DATABASEBROWSER);
    int view_handle = core_alloc_free_space(sizeof(struct cider_browser_view));
    struct cider_browser_view *view =
        view_handle > 0 ? core_get_data_pinned(view_handle) : NULL;
    struct gui_synclist list;
    struct ui_scene scene = {0};
    gui_synclist_init(&list, name, view, false, 1, NULL);
    gui_synclist_set_icon_callback(&list, icon);
    gui_synclist_set_row_callbacks(&list, kind, NULL, NULL);
    root();
    if (!depth && !show_covers) cider_remote_home();
    ahead_previous = history[depth].selected;
    cider_warm_art(NULL, 0, "S");
    agents_nav_take(AGENTS_NAV_TRACKS);
    if (depth && !show_covers)
        cider_page_open(history[depth].route);
    int result = GO_TO_PREVIOUS;
    remote_return = false;
    bool play_pending = false;
    for (;;)
    {
        if (play_pending)
        {
            int status = cider_play_status();
            if (status > 0) { result = GO_TO_WPS; break; }
            if (status < 0)
            {
                play_pending = false;
                splashf(HZ, "%s could not play the selected item", cider_remote_name());
            }
        }
        if (show_covers)
        {
            show_covers = false;
            ui_scene_cancel(&scene);
            gui_synclist_scroll_stop(&list);
            if (!cover_nav_active()) cover_nav_begin(GO_TO_CIDER);
            if (cover_parent_depth < 0) cover_parent_depth = depth;
            int ret = cider_covers();
            if (ret == CIDER_COVERS_REMOTE_RETURN ||
                agents_nav_pending() == AGENTS_NAV_BACK)
            {
                result = toggle_covers();
                break;
            }
            if (ret != GO_TO_PREVIOUS)
            {
                result = ret;
                break;
            }
            else
            {
                int parent = cover_nav_finish();
                close_covers(true);
                if (parent != GO_TO_CIDER)
                {
                    result = parent == GO_TO_WPS ? GO_TO_PREVIOUS : parent;
                    break;
                }
                if (depth) cider_page_open(history[depth].route);
            }
        }
        int count = !cider_available() ? 0
                    : depth            ? cider_page_total()
                                       : root_count;
        if (!play_pending && depth && count > 0 &&
            (!strcmp(history[depth].route, "random") ||
             !strcmp(history[depth].route, "shuffle")) &&
            cider_page_row(0, &selected_row) && cider_play(selected_row.route))
        {
            depth--;
            cider_page_open(history[depth].route);
            play_pending = true;
            continue;
        }
        bool albums = cider_available() && count > 0 && depth && (!strncmp(history[depth].route, "albums", 6) ||
                                !strcmp(history[depth].route, "root/Album"));
        bool header = cider_available() && count > 0 && depth &&
                      !strncmp(history[depth].route, "album/", 6);
        if (header && history[depth].art[0])
            cider_pin_art(history[depth].art);
        else
            cider_unpin_art();

        if (header && view)
        {
            struct browser_db_header *album = &history[depth].header;
            album->tracks = count;
            browser_album_details(album, view->album_detail,
                                  sizeof(view->album_detail));
            view->header = (struct list_header) {
                .title = album->name,
                .subtitle = album->artist,
                .detail = view->album_detail,
                .cover = header_art,
                .data = view,
            };
            gui_synclist_set_header(&list, &view->header);
        }
        else
            gui_synclist_set_header(&list, NULL);

        bool artists = depth && count > 0 && global_settings.db_artistart &&
                       cider_page_row(0, &callback_row) && callback_row.kind == CIDER_ROW_ARTIST;
        bool covers_visible = (albums && global_settings.db_albumart && view) || artists;
        gui_synclist_set_albumart_callback(&list, artists ? artist_art : covers_visible ? art : NULL);
        list.callback_prefetch_art = artists ? artist_prefetch : NULL;
        gui_synclist_set_row_height(&list,
                                    covers_visible ? global_settings.db_art_row_height : 0);
        char title[160];
        if (depth)
            strlcpy(title, history[depth].title, sizeof(title));
        else
            snprintf(title, sizeof(title), "%s Remote", cider_remote_name());
        gui_synclist_set_title(&list, title, Icon_Audio);
        list_notice = cider_list_notice(cider_connected(), cider_available(), count,
                                        depth && cider_page_stale());
        empty_list = list_notice != NULL;
        gui_synclist_set_nb_items(&list, empty_list ? 1 : MAX(0, count));
        gui_synclist_select_item(&list, history[depth].selected);
        if ((albums || artists) && count > 0)
        {
            static char keys[16][65]; /* UI stack is tight */
            const char *window[16]; unsigned int n = 0;
            int selected = history[depth].selected;
            if (selected != ahead_previous)
            {
                ahead_direction = selected > ahead_previous ? 1 : -1;
                ahead_previous = selected;
            }
            int first = MAX(0, selected - 2);
            for (int i = first; i < MIN(count, first + 7); i++)
                if (cider_page_row(i, &callback_row))
                {
                    snprintf(keys[n], sizeof keys[n], "%s", callback_row.art);
                    window[n] = keys[n]; n++;
                }
            unsigned int extra = cider_read_ahead_count(61, 7);
            for (unsigned int j = 0; j < extra && n < ARRAYLEN(window); j++)
            {
                int i = ahead_direction > 0 ? first + 7 + j : first - 1 - j;
                if (i >= 0 && i < count && cider_page_row(i, &callback_row))
                {
                    snprintf(keys[n], sizeof keys[n], "%s", callback_row.art);
                    window[n] = keys[n]; n++;
                }
            }
            cider_read_ahead(window, n, "S");
            cider_warm_art(window, MIN(n, 7), "S");
            const char *previews[5];
            unsigned int missing = 0;
            for (unsigned int i = 0; i < n && missing < ARRAYLEN(previews); i++)
                if (!cider_art_local(window[i], "S") && !cider_has_art(window[i], "XS"))
                    previews[missing++] = window[i];
            if (missing) cider_request_xs(previews, missing);
        }
        if (scene.capturing)
        {
            gui_synclist_draw_settled(&list);
            ui_scene_start(&scene);
        }
        else
        {
            gui_synclist_draw(&list);
            ui_scene_update(&scene, true);
        }
        int action;
        long refresh_at = current_tick + HZ / 10;
        do
        {
            /* Keep the library's normal refresh cadence. Scene frames
             * don't need to repeat the row callbacks or art read-ahead. */
            int timeout = MAX(0, refresh_at - current_tick);
            bool redrew = list_do_action(CONTEXT_ID3DB,
                                         ui_scene_timeout(&scene, timeout),
                                         &list, &action);
            ui_scene_update(&scene, redrew);
        } while (action == ACTION_NONE && scene.handle > 0 &&
                 TIME_BEFORE(current_tick, refresh_at));
        int previous = depth;
        history[depth].selected = gui_synclist_get_sel_pos(&list);
        enum agents_nav_screen nav = agents_nav_pending();
        if (nav == AGENTS_NAV_AGENTS)
        {
            result = GO_TO_AGENTS;
            break;
        }
        if (nav == AGENTS_NAV_WPS)
        {
            result = GO_TO_WPS;
            break;
        }
        if (nav == AGENTS_NAV_COVERS)
        {
            agents_nav_take(nav);
            cider_remote_source();
            if (!cider_available()) { result = GO_TO_ALBUM_COVERS; break; }
            show_covers = true;
            continue;
        }
        if (nav == AGENTS_NAV_BACK)
        {
            if (cover_parent_depth >= 0)
            {
                result = toggle_covers();
                break;
            }
            agents_nav_take(nav);
            action = ACTION_STD_CANCEL;
        }
        if (action == ACTION_STD_QUICKSCREEN)
        {
            if (quick_screen_current(action, NULL) == QUICKSCREEN_IN_USB)
            {
                result = GO_TO_ROOT;
                break;
            }
            continue;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            play_pending = false;
            if (depth <= browse_floor)
            {
                /* Cider's root is a Home child, not a history trampoline. */
                result = browse_floor ? GO_TO_PREVIOUS : GO_TO_ROOT;
                break;
            }
            depth--;
            cider_page_open(history[depth].route);
        }
        else if (action == ACTION_TREE_WPS || action == ACTION_REMOTE_WPS)
        {
            result = GO_TO_WPS;
            break;
        }
        else if (action == ACTION_STD_OK && !play_pending && cider_available() && !empty_list &&
                 get_row(history[depth].selected, &selected_row))
        {
            if (!strcmp(selected_row.route, "cache"))
            {
                if (cider_store_debug_show()) { result = GO_TO_ROOT; break; }
                cider_page_open(history[depth].route);
            }
            else if (!strcmp(selected_row.route, "wps"))
            {
                result = GO_TO_WPS;
                break;
            }
            else if (!strcmp(selected_row.route, "covers"))
            {
                show_covers = true;
            }
            else if (!strcmp(selected_row.route, "root/Search"))
            {
                if (search(&selected_row))
                {
                    if (selected_row.kind == CIDER_ROW_SONG)
                    {
                        if (cider_play_in(selected_row.route, search_route,
                                          search_index))
                        {
                            result = GO_TO_WPS; break;
                        }
                    }
                    else enter(&selected_row);
                }
                else if (depth)
                    cider_page_open(history[depth].route);
            }
            else if (!strncmp(selected_row.route, "play/", 5) ||
                     !strcmp(selected_row.route, "root/Random%20Album") ||
                     !strcmp(selected_row.route, "root/Shuffle%20Songs"))
            {
                if (!strncmp(selected_row.route, "play/", 5))
                {
                    /* A song inside a list plays the list from that song on */
                    if (cider_play_in(selected_row.route,
                                      depth ? history[depth].route : "",
                                      history[depth].selected))
                    {
                        result = GO_TO_WPS; break;
                    }
                }
                else
                {
                    snprintf(selected_row.route, sizeof selected_row.route,
                             "%s",
                             !strcmp(selected_row.route, "root/Random%20Album")
                                 ? "random"
                                 : "shuffle");
                    enter(&selected_row);
                }
            }
            else if (!strncmp(selected_row.route, "input?field=", 12))
            {
                char value[64] = "", encoded[270];
                char field[32];
                snprintf(field, sizeof field, "%s", selected_row.route + 12);
                if (dialog_input(value, sizeof value) == 0)
                {
                    if (!strcmp(field, "range") ||
                        !strcmp(field, "artistrange"))
                    {
                        char end[16] = "";
                        if (dialog_input(end, sizeof end) != 0)
                            continue;
                        char start[16];
                        snprintf(start, sizeof start, "%.15s", value);
                        snprintf(value, sizeof value, "%s-%s", start, end);
                    }
                    escape(encoded, sizeof encoded, value);
                    const char *target =
                        !strcmp(field, "title") || !strcmp(field, "rating")
                            ? "songs"
                        : !strcmp(field, "artist")      ? "artists"
                        : !strcmp(field, "albumartist") ? "albumartists"
                                                        : "albums";
                    if (!strcmp(field, "artistrange"))
                    {
                        snprintf(field, sizeof field, "range");
                        target = "albumartists";
                    }
                    bool contains = !strcmp(field,"album") ||
                                    !strcmp(field,"artist") ||
                                    !strcmp(field,"albumartist") ||
                                    !strcmp(field,"title");
                    snprintf(selected_row.route, sizeof selected_row.route,
                             "%s?%s%s=%s", target, field,
                             contains ? "~" : "", encoded);
                    enter(&selected_row);
                }
            }
            else
                enter(&selected_row);
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = GO_TO_ROOT;
            break;
        }
        if (!show_covers)
            animate_depth(&scene, previous);
    }
    if (cover_parent_depth >= 0)
    {
        if (result == GO_TO_WPS)
        {
            depth = cover_parent_depth;
            cover_nav_set_playing(true);
            cover_suspended = true;
        }
        else
            close_covers(false);
    }
    carousel_forget_resume();
    ui_scene_cancel(&scene);
    gui_synclist_scroll_stop(&list);
    gui_synclist_forget(&list);
    if (view)
    {
        core_put_data_pinned(view);
        core_free(view_handle);
    }
    cider_unpin_art();
    pop_current_activity();
    agents_nav_leave(AGENTS_NAV_TRACKS);
    /* Only a carousel that opened WPS keeps a resumable browser parent. */
    if (!cover_suspended)
    {
        depth = 0;
        history[0].selected = 0;
    }
#ifdef HAVE_CRUMBS
    crumb(CR_CIDER_OUT, (uint32_t)result);
#endif
    return result;
}
