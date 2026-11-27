/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Text search across the tag database: type, and matching titles, albums and
 * album artists appear. Reached from the root menu (off by default) and from
 * Music; both rows are hidden unless the database is held in RAM.
 *
 * The stock menu tree uses the full-page letter strip below. Other themes
 * use widgets/search_dialog.c and its provider.
 *
 * The title scan walks the sorted tag file instead of the master index.
 * The generic dialog scans after typing settles. Stock scans after each edit.
 *
 * Selecting a result returns a GO_TO_* code for root_menu.c to dispatch.
 *
 * Three settings shape it, under Library > Music > Search: how many results
 * are kept, how many letters must be typed before a scan runs, and which of
 * the three tags leads.
 *
 * Parts, in order:
 *   - the matches
 *   - the scan
 *   - the provider
 *   - acting on a result
 ****************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "lang.h"
#include "draw/gloss.h"
#include "draw/round_rect.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "ui/native_theme_runtime.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "widgets/search_dialog.h"
#include "widgets/splash.h"
#include "database/tagcache.h"
#include "database/db_spoken.h"       /* which albums and artists are books */
#include "playlist/playlist.h"
#include "screens/browse/browser_db.h"   /* the two enter_..._on_next_load */
#include "system/activity.h"
#include "root_menu.h"                /* the GO_TO_* codes returned */
#include "db_search.h"

/* The "bitmaps/" prefix is required, not cosmetic: dependencies are generated
 * with -MG, so a header that does not exist yet is recorded at the path as
 * written. Only this spelling matches the rule bitmaps.make gives it. */
#include "bitmaps/podbox_icon_album.h"
#include "bitmaps/podbox_icon_artist.h"
#include "bitmaps/podbox_icon_track.h"

#define MAX_MATCHES  200
#define NAME_ARENA   (8 * 1024)

/* Which tags are searched, one row per value of the order setting. Results are
 * appended in scan order and never sorted, so the row read is also the order
 * they appear in. Title is per track and dominates the cost; the other two are
 * unique-valued tags whose blobs hold one entry per distinct value, so they are
 * nearly free.
 *
 * tag_artist is deliberately absent. It is the per-track artist, so on any
 * compilation it produces a row per guest that leads to the same albums the
 * album artist already does -- duplicates that push real results off a list
 * this short. What a search means by "artist" is the album artist. */
static const int search_tags[DB_SEARCH_ORDER_COUNT][3] = {
    { tag_title,       tag_album,       tag_albumartist },
    { tag_title,       tag_albumartist, tag_album       },
    { tag_album,       tag_title,       tag_albumartist },
    { tag_album,       tag_albumartist, tag_title       },
    { tag_albumartist, tag_album,       tag_title       },
    { tag_albumartist, tag_title,       tag_album       },
};

/* Podcasts' rows, by podcast_search_order: a show's album artist is its
 * publisher, which Podcasts has no list of, so it is not searched */
static const int podcast_tags[2][3] = {
    { tag_title, tag_album, -1 },
    { tag_album, tag_title, -1 },
};

/* Both count settings are indices into evenly spaced choice lists, so the
 * value is arithmetic rather than a second table to hold in step with
 * settings_list.c. */
int db_search_max_results(void)
{
    return 25 * (global_settings.db_search_max_rows + 1);
}

int db_search_min_letters(void)
{
    return global_settings.db_search_min_letters + 1;
}

/* The query the screen last closed on, so reopening resumes rather than
 * starting blank. Deliberately not a setting: it is a convenience within a
 * session, not something worth persisting across a boot. */
static char last_query[SEARCH_MAX_QUERY + 1];

/* ---- the matches -------------------------------------------------------- */

static struct match {
    uint16_t name_off;          /* into name_arena */
    uint8_t  tag;
    int32_t  seek;              /* tag seek: what the browser is armed with */
    int32_t  idx_id;            /* master index id: what a track is played by */
} matches[MAX_MATCHES];

static int  match_count;
static char name_arena[NAME_ARENA];
static int  arena_used;
static int  row_limit;          /* the max-rows setting, taken once per scan */

static bool add_match(int tag, const char *name, int32_t seek, int32_t idx_id)
{
    int len = strlen(name) + 1;

    if (match_count >= row_limit || arena_used + len > NAME_ARENA)
        return false;

    matches[match_count].name_off = arena_used;
    matches[match_count].tag = tag;
    matches[match_count].seek = seek;
    matches[match_count].idx_id = idx_id;
    match_count++;

    memcpy(&name_arena[arena_used], name, len);
    arena_used += len;
    return true;
}

/* ---- the scan ----------------------------------------------------------- */

/* Re-run the whole search for `query`.
 *
 * The loop does not yield. What keeps this off the critical path is the settle
 * delay in front of it, not chunking: a scan only starts once the query has
 * stood still, and on a library this size it is over in a fraction of that.
 *
 * The clause machinery is deliberately unused. A clause makes tagcache walk
 * the master index -- one 96-byte index_entry per track, plus a scattered read
 * into the string blob for each. With no filter and no clause,
 * tagcache_get_next() instead crawls the tag file sequentially: the same
 * strings for a fraction of the memory traffic, and already sorted. */
/* The arming is a one-shot, spent by the next db_search_run(). It has to be,
 * because the row that armed it and the main menu's own row both arrive at the
 * same screen through the same function -- so "nothing armed" and "the whole
 * library" are deliberately the same state, and a scope left standing would
 * silently narrow the next search someone opened. */
static int armed_scope = DB_SEARCH_ALL;
static int active_scope = DB_SEARCH_ALL;

void db_search_arm_scope(int scope)
{
    armed_scope = scope;
}

/* Whether a row this scan turned up belongs on the side being searched.
 *
 * A title is per track, so the tag itself says. Album and album artist are
 * unique-valued -- their tag files hold one entry per distinct string with no
 * track behind it -- so db_spoken answers for them. An album artist with books
 * and music is found on both sides, and so is an album name that a book and a
 * music album share; the same goes for books and podcasts. */
static bool match_in_scope(int tag, const struct tagcache_search *tcs)
{
    int kind = active_scope == DB_SEARCH_PODCAST ? 2 : 1;
    bool spoken;

    if (active_scope == DB_SEARCH_ALL
        || (active_scope == DB_SEARCH_MUSIC && !db_spoken_apart()))
        return true;

    if (active_scope != DB_SEARCH_MUSIC)
    {
        if (tag == tag_albumartist)
            return db_spoken_artist_has_book(tcs->result_seek, kind);
        if (tag == tag_album)
            return db_spoken_album_has_book(tcs->result_seek, kind);
        if (!db_spoken_group_tag(tag))
            return tagcache_get_numeric(tcs, tag_virt_spoken) == kind;
    }
    if (db_spoken_group_tag(tag))
        spoken = db_spoken_group_is_book(tag, tcs->result_seek);
    else if (tag == tag_album || tag == tag_albumartist)
        spoken = false; /* no album tables: no track behind the result */
    else
        spoken = db_spoken_kind_apart(tagcache_get_numeric(tcs,
                                                           tag_virt_spoken));

    return spoken == (active_scope != DB_SEARCH_MUSIC);
}

/* Whether `query` occurs in `text`, ignoring case. A typed apostrophe also
 * matches the typographic one (U+2019): the wheel offers only the first, and
 * tags are full of the second. */
static bool text_matches(const char *text, const char *query)
{
    if (!strchr(query, '\''))
        return strcasestr(text, query) != NULL;

    for (; *text; text++)
    {
        const char *t = text;
        const char *q = query;

        for (; *q; q++)
        {
            if (*q == '\'' && !strncmp(t, "\xe2\x80\x99", 3))
                t += 3;
            else if (tolower((unsigned char)*t) == tolower((unsigned char)*q))
                t++;
            else
                break;
        }

        if (!*q)
            return true;
    }

    return false;
}

static int run_search(const char *query, void *ctx)
{
    char buf[TAGCACHE_BUFSZ];
    struct tagcache_search tcs;
    const int *tags = active_scope == DB_SEARCH_PODCAST
                      ? podcast_tags[global_settings.podcast_search_order]
                      : search_tags[active_scope == DB_SEARCH_SPOKEN
                                    ? global_settings.audiobook_search_order
                                    : global_settings.db_search_order];
    unsigned int i;

    (void)ctx;

    match_count = 0;
    arena_used = 0;

    /* The setting is a second, lower ceiling; MAX_MATCHES and NAME_ARENA stay
     * the hard array bounds, so no setting can overrun them. */
    row_limit = MIN(db_search_max_results(), MAX_MATCHES);

    /* Too short to bother with -- and, since the threshold is at least one
     * letter, this is also the empty-query case. Everything above is already
     * cleared, so a query backspaced below the threshold empties the results
     * rather than leaving the last scan's on screen. */
    if ((int)strlen(query) < db_search_min_letters())
        return 0;

    for (i = 0; i < ARRAYLEN(search_tags[0]); i++)
    {
        if (tags[i] < 0 || !tagcache_search(&tcs, tags[i]))
            continue;

        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            if (text_matches(buf, query)
                && match_in_scope(tags[i], &tcs)
                && !add_match(tags[i], buf, tcs.result_seek, tcs.idx_id))
                break;
        }

        tagcache_search_finish(&tcs);
    }

    return match_count;
}

/* ---- the provider ------------------------------------------------------- */

static const char *match_text(int i, void *ctx)
{
    (void)ctx;
    return &name_arena[matches[i].name_off];
}

/* The icon says which tag matched, so the row is just the text. Album artist
 * shares the artist icon -- the distinction is not worth a third glyph. */
static const struct bitmap *match_icon(int i, void *ctx)
{
    (void)ctx;
    switch (matches[i].tag)
    {
        case tag_album:       return &bm_podbox_icon_album;
        case tag_artist:
        case tag_albumartist: return &bm_podbox_icon_artist;
        default:              return &bm_podbox_icon_track;
    }
}

/* ---- acting on a result ------------------------------------------------- */

/* A title row is one track, so it plays rather than browsing to itself. The
 * path is not held in the match -- only the master index id is -- because the
 * filename is the one tag the ramcache does not keep as a string. */
static int play_track(const struct match *m)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    bool got;

    if (!tagcache_search(&tcs, tag_filename))
        return GO_TO_PREVIOUS;
    got = tagcache_retrieve(&tcs, m->idx_id, tag_filename, path, sizeof(path));
    tagcache_search_finish(&tcs);

    if (!got || playlist_create(NULL, NULL) < 0)
        return GO_TO_PREVIOUS;
    if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true) < 0)
        return GO_TO_PREVIOUS;

    playlist_start(0, 0, 0);
    return GO_TO_WPS;
}

static int act_on(const struct match *m)
{
    const char *name = &name_arena[m->name_off];

    switch (m->tag)
    {
        case tag_album:
            /* straight into that album's tracks, as Album covers does */
            browser_db_enter_album_tracks_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        case tag_albumartist:
            /* straight into that artist's albums, as Artist portraits does */
            browser_db_enter_artist_albums_on_next_load(m->seek, name);
            return GO_TO_ALBUM_COVERS_TRACKS;
        default:
            return play_track(m);
    }
    return GO_TO_PREVIOUS;
}

/* ---- the stock search page ---------------------------------------------- */

#define STOCK_SEARCH_ROW 55
#define STOCK_SEARCH_FIELD_X 28
#define STOCK_SEARCH_FIELD_W 65
#define STOCK_SEARCH_WELL_X 105
#define STOCK_SEARCH_WELL_W 188

/* The letters are lowercase in the query. Space is on the wheel because
 * the physical Play button belongs to playback. */
static const char stock_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ_";

struct stock_match {
    int tag, seek, artist, idx_id, count;
    bool spoken;
};

struct stock_cursor {
    struct stock_match match;
    char name[TAGCACHE_BUFSZ];
    int next;
    bool ready;
};

static struct {
    struct stock_match rows[MAX_MATCHES];
    char query[SEARCH_MAX_QUERY + 1];
    int count, selected, top, letter;
    int font, subfont;
    bool editing, returning;
    struct ui_theme_gradient selection;
} stock_search;

/* Search lists names beginning with the query ahead of other matches. */
static bool stock_name_matches(const char *name, bool prefix)
{
    const char *query = stock_search.query;
    bool starts = !strncasecmp(name, query, strlen(query));
    return prefix ? starts : !starts && text_matches(name, query);
}

/* Read one match from each ordered stream. Album rows use the album/artist
 * pair because two albums named Greatest Hits have separate tracks. */
static bool stock_next(struct stock_cursor *c, struct tagcache_search *tracks,
                       bool prefix)
{
    struct stock_match *m = &c->match;

    switch (m->tag)
    {
        case tag_title:
            while (tagcache_get_next(tracks, c->name, sizeof(c->name)))
            {
                if (tagcache_entry_string(tracks->idx_id, tag_title, c->name,
                                          sizeof(c->name)) &&
                    stock_name_matches(c->name, prefix))
                {
                    m->seek = tracks->result_seek;
                    m->idx_id = tracks->idx_id;
                    return true;
                }
            }
            return false;

        case tag_album:
        {
            struct tagcache_album album;
            while (tagcache_album_get(c->next++, &album))
            {
                if (!tagcache_seek_string(tag_album, album.album_seek,
                                          c->name, sizeof(c->name)) ||
                    !stock_name_matches(c->name, prefix))
                    continue;
                m->seek = album.album_seek;
                m->artist = album.artist_seek;
                m->count = album.tracks;
                m->spoken = album.spoken == album.tracks;
                return true;
            }
            return false;
        }

        default:
        {
            struct tagcache_artist artist;
            while (tagcache_artist_get(c->next++, &artist))
            {
                if (!tagcache_seek_string(tag_albumartist, artist.seek,
                                          c->name, sizeof(c->name)) ||
                    !stock_name_matches(c->name, prefix))
                    continue;
                m->seek = artist.seek;
                m->count = artist.albums;
                m->spoken = artist.spoken_albums == artist.albums;
                return true;
            }
            return false;
        }
    }
}

static bool stock_db_ready(void)
{
    return tagcache_is_in_ram() && tagcache_is_usable()
        && tagcache_search_ready();
}

static void stock_scan_pass(bool prefix)
{
    static struct stock_cursor cursor[3];
    struct tagcache_search tracks;
    if (!tagcache_search(&tracks, tag_title))
        return;
    memset(cursor, 0, sizeof(cursor));
    cursor[0].match.tag = tag_title;
    cursor[1].match.tag = tag_album;
    cursor[2].match.tag = tag_albumartist;
    cursor[2].match.artist = -1;

    for (size_t i = 0; i < ARRAYLEN(cursor); i++)
        cursor[i].ready = stock_next(&cursor[i], &tracks, prefix);

    while (stock_search.count < MAX_MATCHES)
    {
        int best = -1;
        for (size_t i = 0; i < ARRAYLEN(cursor); i++)
            if (cursor[i].ready &&
                (best < 0 || strcasecmp(cursor[i].name, cursor[best].name) < 0))
                best = i;
        if (best < 0)
            break;
        struct stock_cursor *c = &cursor[best];
        stock_search.rows[stock_search.count++] = c->match;
        if (stock_search.count < MAX_MATCHES)
            c->ready = stock_next(c, &tracks, prefix);
    }
    tagcache_search_finish(&tracks);
}

static void stock_scan(void)
{
    stock_search.count = 0;
    if (!stock_search.query[0] || !stock_db_ready())
        return;
    stock_scan_pass(true);
    if (stock_search.count < MAX_MATCHES)
        stock_scan_pass(false);
}

static enum stock_art_id stock_row_text(int row, char *title, size_t title_len,
                                        char *sub, size_t sub_len)
{
    if (row == 0)
    {
        strlcpy(title, str(LANG_DB_SEARCH), title_len);
        if (stock_search.query[0])
            snprintf(sub, sub_len, str(LANG_FRUIT_SEARCH_QUERY),
                     stock_search.query);
        else
            strlcpy(sub, str(LANG_FRUIT_SEARCH_EMPTY), sub_len);
        return STOCK_ART_SEARCHRESULT_SEARCH;
    }

    const struct stock_match *m = &stock_search.rows[row - 1];
    title[0] = sub[0] = '\0';
    switch (m->tag)
    {
        case tag_title:
            tagcache_entry_string(m->idx_id, tag_title, title, title_len);
            tagcache_entry_string(m->idx_id, tag_artist, sub, sub_len);
            return STOCK_ART_SEARCHRESULT_SONG;
        case tag_album:
            tagcache_seek_string(m->tag, m->seek, title, title_len);
            snprintf(sub, sub_len, str(m->count == 1 ? LANG_FRUIT_ONE_SONG
                                                    : LANG_FRUIT_SONG_COUNT),
                     m->count);
            return STOCK_ART_SEARCHRESULT_ALBUM;
        default:
            tagcache_seek_string(m->tag, m->seek, title, title_len);
            snprintf(sub, sub_len, str(m->count == 1 ? LANG_FRUIT_ONE_ALBUM
                                                    : LANG_FRUIT_ALBUM_COUNT),
                     m->count);
            return STOCK_ART_SEARCHRESULT_ARTIST;
    }
}

static void stock_strip(void)
{
    const int y = LCD_HEIGHT - 68;
    const struct gloss_surface surface = {
        lcd_framebuffer_default.fb_ptr, LCD_WIDTH, 0, 0, LCD_WIDTH, LCD_HEIGHT,
    };
    const struct gloss_fill panel = {
        0xffb8b8b8, 0xff777777, 0xf20a0a0a, 0xff484848,
    };
    const struct gloss_fill well = {
        0xff676767, 0xff080808, 0xff2d2d2d, 0xff373737,
    };
    const struct gloss_fill field = {
        0xff757575, 0xfff3f3f3, 0xffffffff, 0xffd2d2d2,
    };
    const struct gloss_fill key = {
        0xff70a5d0, 0xff3989be, 0xff125698, 0xff164e7b,
    };
    struct viewport vp = {
        .x = STOCK_SEARCH_WELL_X, .y = y + 9,
        .width = STOCK_SEARCH_WELL_W, .height = 26,
        .font = stock_search.font, .drawmode = DRMODE_FG,
        .fg_pattern = LCD_WHITE, .buffer = &lcd_framebuffer_default,
    };

    gloss_shadow(&surface, 20, y, 280, 46, 6, 5, 2, 85);
    gloss_rect(&surface, 20, y, 280, 46, 6, 6, &panel);
    gloss_rect(&surface, STOCK_SEARCH_FIELD_X, y + 9,
               STOCK_SEARCH_FIELD_W, 25, 4, 4, &field);
    gloss_rect(&surface, STOCK_SEARCH_WELL_X, y + 9,
               STOCK_SEARCH_WELL_W, 26, 6, 6, &well);

    int positions[sizeof(stock_keys)] = {0};
    struct viewport *old = lcd_set_viewport(&vp);
    for (size_t i = 0; i < sizeof(stock_keys) - 1; i++)
    {
        char letter[2] = {stock_keys[i], '\0'};
        int width;
        lcd_getstringsize(letter, &width, NULL);
        positions[i + 1] = positions[i] + width + 5;
    }
    int first = positions[stock_search.letter];
    int end = positions[stock_search.letter + 1];
    int offset = MIN(MAX(0, (first + end - vp.width) / 2),
                     MAX(0, positions[sizeof(stock_keys) - 1] - vp.width));
    gloss_rect(&surface, vp.x + first - offset, vp.y + 2, end - first, 22,
               4, 4, &key);

    for (size_t i = 0; i < sizeof(stock_keys) - 1; i++)
    {
        int x = positions[i] - offset;
        if (positions[i + 1] <= offset || x >= vp.width)
            continue;
        char letter[2] = {stock_keys[i], '\0'};
        lcd_putsxy(x + 2, 4, letter);
    }

    vp.x = STOCK_SEARCH_FIELD_X + 3;
    vp.width = STOCK_SEARCH_FIELD_W - 6;
    vp.fg_pattern = LCD_BLACK;
    const char *query = stock_search.query;
    int width;
    lcd_getstringsize(query, &width, NULL);
    while (width >= vp.width)
        lcd_getstringsize(++query, &width, NULL);
    lcd_putsxy(0, 4, query);
    lcd_vline(width, 4, 20);
    lcd_set_viewport(old);
}

static void stock_paint(void)
{
    int top = sb_visible_height();
    int height = LCD_HEIGHT - top;
    struct viewport body = {
        .x = 0, .y = top, .width = LCD_WIDTH, .height = height,
        .font = stock_search.font, .drawmode = DRMODE_FG,
        .fg_pattern = LCD_BLACK, .bg_pattern = LCD_WHITE,
        .buffer = &lcd_framebuffer_default,
    };
    struct ui_rect clip = {0, top, LCD_WIDTH, height};
    struct viewport *old = lcd_set_viewport(&body);
    lcd_clear_viewport();

    int visible = MAX(1, height / STOCK_SEARCH_ROW);
    if (stock_search.selected < stock_search.top)
        stock_search.top = stock_search.selected;
    if (stock_search.selected >= stock_search.top + visible)
        stock_search.top = stock_search.selected - visible + 1;
    if (stock_search.editing)
        stock_search.top = 0;

    for (int y = 0, row = stock_search.top;
         y < height && row <= stock_search.count; y += STOCK_SEARCH_ROW, row++)
    {
        char title[TAGCACHE_BUFSZ], sub[TAGCACHE_BUFSZ];
        enum stock_art_id art =
            stock_row_text(row, title, sizeof(title), sub, sizeof(sub));
        stock_art_draw(art, 0, top + y, clip);
        bool selected = !stock_search.editing && row == stock_search.selected;

        if (selected)
        {
            for (int dy = 0; dy < STOCK_SEARCH_ROW; dy++)
            {
                lcd_set_foreground(FB_SCALARPACK(ui_theme_gradient_at(
                    &stock_search.selection, dy, STOCK_SEARCH_ROW, 0x0065d3)));
                lcd_hline(STOCK_SEARCH_ROW, LCD_WIDTH - 1, y + dy);
            }
            stock_art_draw(STOCK_ART_SYSTEM_SUBMENU, LCD_WIDTH - 17,
                           top + y + 20, clip);
        }
        else
        {
            lcd_set_foreground(LCD_RGBPACK(216, 216, 216));
            lcd_hline(STOCK_SEARCH_ROW, LCD_WIDTH - 1, y + STOCK_SEARCH_ROW - 1);
        }

        struct viewport text = body;
        text.x = STOCK_SEARCH_ROW + 10;
        text.width = LCD_WIDTH - text.x - 18;
        lcd_set_viewport(&text);
        lcd_set_foreground(selected ? LCD_WHITE : LCD_BLACK);
        lcd_setfont(stock_search.font);
        lcd_putsxy(0, y + 8, title);
        lcd_set_foreground(selected ? LCD_WHITE : LCD_RGBPACK(102, 102, 102));
        lcd_setfont(stock_search.subfont);
        lcd_putsxy(0, y + 28, sub);
        lcd_set_viewport(&body);
    }

    int rows_h = (stock_search.count + 1) * STOCK_SEARCH_ROW;
    if (rows_h > height)
    {
        int thumb = MAX(38, height * height / rows_h);
        int y = (height - thumb - 6) * stock_search.top /
                MAX(1, stock_search.count + 1 - visible);
        lcd_set_foreground(LCD_RGBPACK(143, 143, 143));
        fill_round_rect(&screens[SCREEN_MAIN], LCD_WIDTH - 5, y + 3, 4,
                        thumb, 2);
    }
    if (stock_search.editing)
        stock_strip();

    char title[64];
    if (stock_search.count)
        snprintf(title, sizeof(title), str(LANG_FRUIT_SEARCH_COUNT),
                 stock_search.count);
    else
        strlcpy(title, str(LANG_DB_SEARCH), sizeof(title));
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    lcd_set_viewport(old);
    sb_update(SCREEN_MAIN, true);
    if (height > 0)
        lcd_update_rect(0, top, LCD_WIDTH, height);
}

static void stock_speak_selection(void)
{
    if (!global_settings.talk_menu)
        return;
    char title[TAGCACHE_BUFSZ], sub[TAGCACHE_BUFSZ];
    stock_row_text(stock_search.selected, title, sizeof(title), sub, sizeof(sub));
    talk_settle();
    talk_name(title, false);
}

static int stock_open_result(void)
{
    const struct stock_match *row = &stock_search.rows[stock_search.selected - 1];
    char name[TAGCACHE_BUFSZ];
    if (row->tag == tag_title)
    {
        const struct match m = {.idx_id = row->idx_id, .tag = tag_title};
        return play_track(&m);
    }
    if (!tagcache_seek_string(row->tag, row->seek, name, sizeof(name)))
        return GO_TO_PREVIOUS;
    if (!row->spoken)
        browser_db_enter_fruit_match_on_next_load(row->tag, row->seek,
                                                 row->artist, name);
    else if (row->tag == tag_album)
        browser_db_enter_artist_album_tracks_on_next_load(row->seek,
                                                         row->artist, name);
    else
        browser_db_enter_artist_albums_on_next_load(row->seek, name);
    return GO_TO_ALBUM_COVERS_TRACKS;
}

static int stock_search_run(void)
{
    char saved_title[MAX_PATH];
    enum themable_icons saved_icon = Icon_NOICON;
    bool saved = sb_get_persistent_title(SCREEN_MAIN, saved_title,
                                         sizeof(saved_title), &saved_icon);
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    stock_search.selection = theme.gradient[UI_GRADIENT_SELECTION];
    stock_search.font = ui_theme_font_load(&theme, UI_FONT_LIST,
                                         global_settings.glyphs_to_cache);
    stock_search.subfont = ui_theme_font_load(&theme, UI_FONT_BODY,
                                            global_settings.glyphs_to_cache);
    int fonts[] = {stock_search.font, stock_search.subfont};
    stock_search.font = fonts[0] >= 0 ? fonts[0] : FONT_UI;
    stock_search.subfont = fonts[1] >= 0 ? fonts[1] : FONT_UI;

    stock_scan();
    stock_search.editing = !stock_search.returning && stock_search.count == 0;
    stock_search.selected = stock_search.returning
        ? MIN(stock_search.selected, stock_search.count) : 0;
    stock_search.returning = false;
    stock_search.top = 0;
    stock_search.letter = 0;
    int ret = GO_TO_PREVIOUS;
    bool ready = stock_db_ready();
    uint32_t generation = tagcache_generation();
    int32_t commit = tagcache_commit_id();

    push_current_activity(ACTIVITY_DB_SEARCH);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    stock_paint();
    while (true)
    {
        int action = get_custom_action(CONTEXT_KEYBOARD | CONTEXT_WHEEL_DETENTS,
                                       HZ / 4, NULL);
        bool edited = false;
        switch (action)
        {
            case ACTION_KBD_UP:
            case ACTION_KBD_DOWN:
            {
                int step = action == ACTION_KBD_UP ? -1 : 1;
                if (stock_search.editing)
                    stock_search.letter = MAX(0, MIN((int)sizeof(stock_keys) - 2,
                                                    stock_search.letter + step));
                else
                {
                    stock_search.selected = MAX(0, MIN(stock_search.count,
                                                    stock_search.selected + step));
                    stock_speak_selection();
                }
                break;
            }
            case ACTION_KBD_SELECT:
                if (stock_search.editing)
                {
                    size_t len = strlen(stock_search.query);
                    if (len < SEARCH_MAX_QUERY)
                    {
                        char key = stock_keys[stock_search.letter];
                        stock_search.query[len] = key == '_' ? ' ' : tolower(key);
                        stock_search.query[len + 1] = '\0';
                        edited = true;
                    }
                }
                else if (stock_search.selected == 0)
                {
                    stock_search.editing = true;
                    stock_search.letter = 0;
                }
                else if (ready && stock_db_ready() &&
                         generation == tagcache_generation() &&
                         commit == tagcache_commit_id())
                {
                    ret = stock_open_result();
                    if (ret != GO_TO_PREVIOUS)
                    {
                        stock_search.returning = true;
                        goto done;
                    }
                }
                break;
            case ACTION_KBD_BACKSPACE:
                if (stock_search.editing && stock_search.query[0])
                {
                    stock_search.query[strlen(stock_search.query) - 1] = '\0';
                    edited = true;
                }
                break;
            case ACTION_KBD_ABORT:
                if (!stock_search.editing || !stock_search.count)
                    goto done;
                stock_search.editing = false;
                stock_speak_selection();
                break;
            case ACTION_KBD_DONE:
                if (stock_search.editing)
                {
                    stock_search.editing = false;
                    stock_speak_selection();
                }
                break;
            case ACTION_STD_CANCEL:
                goto done;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = GO_TO_ROOT;
                    goto done;
                }
                break;
        }

        bool now_ready = stock_db_ready();
        uint32_t now_generation = tagcache_generation();
        int32_t now_commit = tagcache_commit_id();
        if (edited || now_ready != ready || now_generation != generation ||
            now_commit != commit)
        {
            ready = now_ready;
            generation = now_generation;
            commit = now_commit;
            stock_scan();
            stock_search.selected = MIN(stock_search.selected, stock_search.count);
        }
        else if (action == ACTION_NONE)
            continue;
        stock_paint();
    }

done:
    talk_shutup();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    sb_set_persistent_title(saved ? saved_title : NULL, saved_icon, SCREEN_MAIN);
    pop_current_activity_without_refresh();
    for (size_t i = 0; i < ARRAYLEN(fonts); i++)
        if (fonts[i] >= 0)
            font_unload(fonts[i]);
    return ret;
}

int db_search_run(void)
{
    static const struct search_provider provider = {
        .activity = ACTIVITY_DB_SEARCH,
        .scan     = run_search,
        .row_text = match_text,
        .row_icon = match_icon,
    };
    struct search_provider p = provider;
    int chosen;

    /* Spend the arming, whether or not this screen goes on to open. */
    active_scope = armed_scope;
    armed_scope = DB_SEARCH_ALL;

    if (ui_wheel_navigation() && active_scope == DB_SEARCH_ALL)
        return stock_search_run();

    /* Both gates the real feature would need. A commit refuses every
     * tagcache_search() for its whole length, so a search screen opened then
     * finds nothing; and off the ramcache every entry costs a seek and a read,
     * which on a disk is not slow but unusable. */
    if (!tagcache_is_usable() || !tagcache_search_ready())
    {
        splash(HZ * 2, "Database busy");
        return GO_TO_PREVIOUS;
    }
    if (!tagcache_is_in_ram())
    {
        splash(HZ * 2, "Needs the database in RAM");
        return GO_TO_PREVIOUS;
    }

    /* str() is not a constant expression, so the title is filled in here
     * rather than in the initialiser above. */
    p.title = str(LANG_DB_SEARCH);

    match_count = 0;
    arena_used = 0;

    chosen = search_dialog_run(&p, NULL, last_query, sizeof(last_query));

    if (chosen == SEARCH_USB)
        return GO_TO_ROOT;        /* USB: root_menu handles the screen */

    if (chosen >= 0)
        return act_on(&matches[chosen]);

    return GO_TO_PREVIOUS;
}
