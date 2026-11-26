/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings > About as the stock iPod Classic 1.1.2 firmware draws it, from
 * its bitmaps and Helvetica: the logo on a band between the used and free
 * space, and under it one of three pages -- the space by kind, the library's
 * counts, and the serial number, model and version. AstraPod's logo and name
 * stand in for Apple's. Select and Next step forward through the pages,
 * Previous back, both round; the wheel does nothing; Menu leaves.
 *
 * Parts: the numbers, the painters, then the loop.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "mv.h"
#include "rbpaths.h"
#include "version.h"
#include "lang.h"
#include "database/tagcache.h"
#include "files/file_index.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"
#include "draw/viewport.h"
#include "ui/native_chrome.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "root_menu.h"
#include "fruit_about.h"
#if defined(IPOD_6G) && !defined(SIMULATOR)
#include "norboot-target.h"
#endif

#define PAGES 3
#define WHITE 0xffffff
/* Where stock draws each part. */
#define BAND_Y 61
#define LOGO_X 128
#define LOGO_Y 40
#define SPACE_TOP 73
#define USED_MID 68
#define FREE_MID 252
/* The bar's pieces start 6 columns left of its ink, which runs from
 * BAR_LEFT up to BAR_RIGHT; its caps are CAP columns wide. */
#define BAR_X 8
#define BAR_Y 135
#define BAR_LEFT 14
#define BAR_RIGHT 306
#define CAP 16
#define RIGHT_CAP_X (BAR_RIGHT + BAR_LEFT - BAR_X - CAP)
#define EDGE_W 2
#define LEGEND_X 14
#define LEGEND_Y 176
#define LEGEND_PITCH 80
#define ROW_TOP 153
#define ROW_PITCH 18
#define BUBBLE_X 144
#define BUBBLE_Y 220
#define BUBBLE_PITCH 13

struct about_info {
    unsigned long long total_kib, free_kib, audio_kib;
    int songs, videos, podcasts, photos, games, contacts;
    char serial[17], model[17], version[48];
};

static struct {
    int page;
    int bold, regular;
    struct about_info info;
} about;

/* ---- the numbers ---- */

/* Each track's size is estimated from its bitrate and length: the database
 * holds no file sizes. */
static void count_library(struct about_info *info)
{
    struct tagcache_search tcs;
    char buf[16];
    if (!tagcache_search(&tcs, tag_length))
        return;
    while (tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        if (tagcache_get_numeric(&tcs, tag_virt_spoken) == 2)
            info->podcasts++;
        else
            info->songs++;
        info->audio_kib +=
            (unsigned long long)tagcache_get_numeric(&tcs, tag_bitrate) *
            tagcache_get_numeric(&tcs, tag_length) / 8192;
    }
    tagcache_search_finish(&tcs);
}

#if defined(IPOD_6G) && !defined(SIMULATOR)
static void copy_field(char *out, const struct SysCfgEntry *entry)
{
    size_t n = 0;
    while (n < sizeof(entry->data) && entry->data[n])
        n++;
    memcpy(out, entry->data, n);
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}
#endif

/* The serial number and model the player's SysCfg holds. Stock leaves a
 * missing serial number blank and shows a missing model as a dash. */
static void read_identity(struct about_info *info)
{
    info->serial[0] = '\0';
    strcpy(info->model, "-");
#if defined(IPOD_6G) && !defined(SIMULATOR)
    struct SysCfg syscfg;
    if (syscfg_read(&syscfg) != 0)
        return;
    int count = MIN(syscfg.header.num_entries, SYSCFG_MAX_ENTRIES);
    for (int i = 0; i < count; i++)
    {
        const struct SysCfgEntry *entry = &syscfg.entries[i];
        if (entry->tag == SYSCFG_TAG_SRNM)
            copy_field(info->serial, entry);
        else if (entry->tag == SYSCFG_TAG_MODN && entry->data[0])
            copy_field(info->model, entry);
    }
#endif
}

static void read_info(struct about_info *info)
{
    memset(info, 0, sizeof(*info));
    sector_t total = 0, free = 0;
    volume_size(IF_MV(0,) &total, &free);
    info->total_kib = total;
    info->free_kib = free;
    count_library(info);
    info->photos = file_index_count(FLAT_IMAGES);
    info->videos = file_index_count(FLAT_VIDEOS);
    read_identity(info);
    /* "AstraPod Beta 0.2.0 (ae20a40148)" leaves no room for its column:
     * the name is in the title and the commit is in the other About, so
     * "Beta 0.2.0" is shown. */
    const char *version = rbversion;
    if (!strncmp(version, "AstraPod ", 9))
        version += 9;
    const char *commit = strstr(version, " (");
    int length = commit ? (int)(commit - version) : (int)strlen(version);
    snprintf(info->version, sizeof(info->version), "%.*s %s", length,
             version, str(LANG_ABOUT_WINDOWS_FORMAT));
}

/* "55.0 MB", "28.6 GB" */
static void format_size(char *buf, size_t size, unsigned long long kib)
{
    bool gib = kib >= 1024ull * 1024;
    unsigned long long unit = gib ? 1024ull * 1024 : 1024;
    snprintf(buf, size, "%u.%u %s", (unsigned)(kib / unit),
             (unsigned)(kib * 10 / unit % 10), gib ? "GB" : "MB");
}

/* The bar's column for `kib` of the volume. */
static int bar_column(unsigned long long kib)
{
    const struct about_info *info = &about.info;
    if (!info->total_kib)
        return BAR_LEFT;
    kib = MIN(kib, info->total_kib);
    return BAR_LEFT + (int)(kib * (BAR_RIGHT - BAR_LEFT) / info->total_kib);
}

/* ---- the painters ---- */

static int text_width(const char *text, int font)
{
    int width = 0;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    return width;
}

static void put_stretched(struct stock_canvas *c, enum stock_art_id id,
                          int x0, int x1)
{
    for (int x = x0; x < x1; x++)
        stock_art_put(c, id, x, BAR_Y);
}

/* Audio, then everything else the volume holds; stock also has video and
 * photos, which AstraPod has no sizes for. A part's left cap shows only
 * when it is the first, and the next part's middle covers the rest of it. */
static void put_bar(struct stock_canvas *c)
{
    static const enum stock_art_id left[] = {
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIOLEFT,
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERLEFT,
    }, middle[] = {
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIOCENTER,
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERCENTER,
    }, right[] = {
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIORIGHT,
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERRIGHT,
    };
    const struct about_info *info = &about.info;
    unsigned long long used = info->total_kib - MIN(info->free_kib,
                                                    info->total_kib);
    int end[2] = { bar_column(MIN(info->audio_kib, used)), bar_column(used) };
    int from = BAR_LEFT, last = -1;
    for (int i = 0; i < 2; from = end[i++])
    {
        if (end[i] <= from)
            continue;
        if (last < 0)
            stock_art_put_part(c, left[i], 0, 0, MIN(CAP, end[i] - BAR_X),
                               stock_art[left[i]].height, BAR_X, BAR_Y);
        put_stretched(c, middle[i], last < 0 ? BAR_X + CAP : from,
                      MIN(end[i], RIGHT_CAP_X));
        last = i;
    }
    int used_end = end[1];
    if (last < 0)
        stock_art_put(c, STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYLEFT, BAR_X,
                      BAR_Y);
    put_stretched(c, STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYCENTER,
                  MAX(used_end, BAR_X + CAP), RIGHT_CAP_X);
    bool full = last >= 0 && used_end >= BAR_RIGHT - CAP / 2;
    stock_art_put(c, full ? right[last] :
                  STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYRIGHT,
                  RIGHT_CAP_X, BAR_Y);
    if (last >= 0 && !full)
        stock_art_put(c, STOCK_ART_CAPACITYVIEW_DETAILED_EDGE,
                      used_end - EDGE_W, BAR_Y);
}

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int top)
{
    stock_canvas_text(c, font, text, x, top, WHITE);
}

static void put_space(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    char amount[16], text[48];
    format_size(amount, sizeof(amount),
                info->total_kib - MIN(info->free_kib, info->total_kib));
    snprintf(text, sizeof(text), str(LANG_ABOUT_USED), amount);
    put_text(c, about.bold, text, USED_MID - text_width(text, about.bold) / 2,
             SPACE_TOP);
    format_size(amount, sizeof(amount), info->free_kib);
    snprintf(text, sizeof(text), str(LANG_ABOUT_FREE), amount);
    put_text(c, about.bold, text, FREE_MID - text_width(text, about.bold) / 2,
             SPACE_TOP);
}

static void put_legend(struct stock_canvas *c)
{
    static const struct {
        enum stock_art_id key;
        int label, x;
    } item[] = {
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_AUDIOLEGEND, LANG_ABOUT_AUDIO, 30 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_VIDEOLEGEND, LANG_ABOUT_VIDEO, 110 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_PHOTOSLEGEND, LANG_ABOUT_PHOTOS,
          191 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_OTHERLEGEND, LANG_ABOUT_OTHER, 270 },
    };
    for (int i = 0; i < (int)ARRAYLEN(item); i++)
    {
        stock_art_put(c, item[i].key, LEGEND_X + i * LEGEND_PITCH, LEGEND_Y);
        put_text(c, about.bold, str(item[i].label), item[i].x, LEGEND_Y);
    }
}

/* The library's counts in two columns, each value against the column's
 * right edge. */
static void put_counts(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    const struct {
        int label, value;
    } cell[] = {
        { LANG_ABOUT_SONGS, info->songs },
        { LANG_ABOUT_VIDEOS, info->videos },
        { LANG_ABOUT_PODCASTS, info->podcasts },
        { LANG_ABOUT_PHOTOS, info->photos },
        { LANG_ABOUT_GAMES, info->games },
        { LANG_ABOUT_CONTACTS, info->contacts },
    };
    static const int label_x[] = { 19, 174 }, value_right[] = { 153, 306 };
    for (int i = 0; i < (int)ARRAYLEN(cell); i++)
    {
        int column = i / 3, top = ROW_TOP + i % 3 * ROW_PITCH;
        char value[12];
        snprintf(value, sizeof(value), "%d", cell[i].value);
        put_text(c, about.bold, str(cell[i].label), label_x[column], top);
        put_text(c, about.regular, value,
                 value_right[column] - text_width(value, about.regular), top);
    }
}

static void put_identity(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    const struct {
        int label;
        const char *value;
    } row[] = {
        { LANG_ABOUT_SERIAL_NUMBER, info->serial },
        { LANG_ABOUT_MODEL, info->model },
        { LANG_ABOUT_VERSION, info->version },
    };
    for (int i = 0; i < (int)ARRAYLEN(row); i++)
    {
        int top = ROW_TOP + i * ROW_PITCH;
        put_text(c, about.bold, str(row[i].label), 18, top);
        put_text(c, about.regular, row[i].value, 192, top);
    }
}

static void compose(struct stock_canvas *c)
{
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    stock_art_put(c, STOCK_ART_SETTINGS_ABOUT_BAND, 0, BAND_Y);
    stock_art_put(c, STOCK_ART_ASTRAPOD_ABOUT_LOGO, LOGO_X, LOGO_Y);
    put_space(c);
    if (about.page == 0)
    {
        put_bar(c);
        put_legend(c);
    }
    else if (about.page == 1)
        put_counts(c);
    else
        put_identity(c);
    for (int i = 0; i < PAGES; i++)
        stock_art_put(c, i == about.page ?
                      STOCK_ART_SETTINGS_ABOUT_BUBBLE_SELECTED :
                      STOCK_ART_SETTINGS_ABOUT_BUBBLE_DESELECTED,
                      BUBBLE_X + i * BUBBLE_PITCH, BUBBLE_Y);
}

/* Composed at 8 bits a channel, as stock composes, in as many rows at a time
 * as the scratch buffer holds, and truncated once. The buffer is free while
 * a settings screen is up; if something holds it the page is not drawn. */
static void paint(void)
{
    static const char owner[] = "About";
    size_t size;
    uint32_t *pixels = app_try_claim_buffer(&size, owner);
    if (!pixels)
        return;
    int top = sb_visible_height();
    int rows = MAX(1, (int)(size / (LCD_WIDTH * sizeof(uint32_t))));
    struct ui_rect clip = { 0, top, LCD_WIDTH, LCD_HEIGHT - top };
    for (int y = top; y < LCD_HEIGHT; y += rows)
    {
        struct stock_canvas band = {
            pixels, 0, y, LCD_WIDTH, MIN(rows, LCD_HEIGHT - y),
        };
        compose(&band);
        stock_canvas_show(&band, clip);
    }
    app_release_buffer(owner);
    lcd_update_rect(0, top, LCD_WIDTH, LCD_HEIGHT - top);
}

/* The page's words in the order they are drawn. */
static void speak_page(void)
{
    const struct about_info *info = &about.info;
    char text[48], amount[16];
    if (!global_settings.talk_menu)
        return;
    talk_force_shutup();
    format_size(amount, sizeof(amount),
                info->total_kib - MIN(info->free_kib, info->total_kib));
    snprintf(text, sizeof(text), str(LANG_ABOUT_USED), amount);
    talk_text_part(text);
    format_size(amount, sizeof(amount), info->free_kib);
    snprintf(text, sizeof(text), str(LANG_ABOUT_FREE), amount);
    talk_text_part(text);
    if (about.page == 1)
    {
        static const int label[] = {
            LANG_ABOUT_SONGS, LANG_ABOUT_VIDEOS, LANG_ABOUT_PODCASTS,
            LANG_ABOUT_PHOTOS, LANG_ABOUT_GAMES, LANG_ABOUT_CONTACTS,
        };
        const int value[] = { info->songs, info->videos, info->podcasts,
                              info->photos, info->games, info->contacts };
        for (int i = 0; i < (int)ARRAYLEN(label); i++)
        {
            talk_text_part(str(label[i]));
            talk_number(value[i], true);
        }
    }
    else if (about.page == 2)
    {
        talk_text_part(str(LANG_ABOUT_SERIAL_NUMBER));
        talk_spell(info->serial, true);
        talk_text_part(str(LANG_ABOUT_MODEL));
        talk_spell(info->model, true);
        talk_text_part(str(LANG_ABOUT_VERSION));
        talk_spell(info->version, true);
    }
}

/* ---- the loop ---- */

static int load_font(const char *name)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), FONT_DIR "/%s.pbf", name);
    int font = font_load_ex(path, 0, global_settings.glyphs_to_cache);
    return font >= 0 ? font : FONT_UI;
}

int fruit_about_screen(void)
{
    char saved_title[MAX_PATH];
    enum themable_icons saved_icon;
    bool has_saved = sb_get_persistent_title(SCREEN_MAIN, saved_title,
                                             sizeof(saved_title), &saved_icon);
    int ret = 0;

    about.page = 0;
    about.bold = load_font("13-helvetica-bold");
    about.regular = load_font("13-helvetica");
    read_info(&about.info);
    sb_set_persistent_title(str(LANG_ROCKBOX_TITLE), Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    sb_update(SCREEN_MAIN, true);
    paint();
    speak_page();

    while (1)
    {
        int action = get_action(CONTEXT_STD, HZ);
        int step = 0;
        if (action == ACTION_STD_MENU)
            break;
        /* Select and Next are OK here, and Previous is Cancel. */
        if (action == ACTION_STD_OK)
            step = 1;
        else if (action == ACTION_STD_CANCEL)
            step = PAGES - 1;
        if (step)
        {
            about.page = (about.page + step) % PAGES;
            paint();
            speak_page();
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    talk_shutup();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    sb_set_persistent_title(has_saved ? saved_title : NULL, saved_icon,
                            SCREEN_MAIN);
    if (about.regular != FONT_UI)
        font_unload(about.regular);
    if (about.bold != FONT_UI)
        font_unload(about.bold);
    return ret;
}
