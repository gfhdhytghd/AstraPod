/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Settings > About as the stock iPod Classic 2.0.5 firmware draws it, from
 * its bitmaps and Helvetica: black glossy status bar, logo on a band between
 * used and free space, and under it one of three pages -- the 4-segment
 * capacity bar (Audio, Video, Photos, Other), the library's seven counts,
 * and the serial number, model and version. AstraPod's logo and name stand
 * in for Apple's. Left, Right and Select turn pages; Menu leaves and Play
 * controls playback.
 *
 * Parts: the numbers, the painters, then the loop.
 ****************************************************************************/

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "dir.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "mv.h"
#include "power.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "version.h"
#include "lang.h"
#include "audio/play_status.h"
#include "database/tagcache.h"
#include "files/file_index.h"
#include "files/path_list.h"
#include "input/action.h"
#include "screens/browse/app_list.h"
#include "screens/extras/extras_bar.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/app_buffer.h"
#include "system/shutdown.h"
#include "draw/viewport.h"
#include "ui/stock_art.h"
#include "widgets/menu.h"
#include "fruit_about.h"
#include "ui/scene.h"
#if defined(IPOD_6G) && !defined(SIMULATOR)
#include "norboot-target.h"
#endif

#define PAGES 3
#define TURN_FRAMES 8
#define TURN_FRAME_MS 34
#define WHITE 0xffffff
#define VALUE_GREY 0xe0e2e4
/* Where stock 2.0.5 draws each part. */
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
#define COUNTS_TOP 135
#define IDENTITY_TOP 153
#define ROW_PITCH 18
#define BUBBLE_X 144
#define BUBBLE_Y 220
#define BUBBLE_PITCH 13
#define CONTACTS_DIR "/Contacts"

enum bar_segment {
    SEG_AUDIO = 0,
    SEG_VIDEO,
    SEG_PHOTOS,
    SEG_OTHER,
    SEG_COUNT
};

struct about_info {
    unsigned long long total_kib, free_kib;
    unsigned long long audio_kib, video_kib, photo_kib;
    int songs, videos, podcasts, itunes_u, photos, games, contacts;
    char serial[17], model[17], version[48];
};

static struct {
    int page;
    int bold, regular;
    struct about_info info;
} about;

/* ---- the numbers ---- */

static bool is_itunes_u_genre(const char *genre)
{
    return genre && (!strcasecmp(genre, "iTunes U") ||
                     !strcasecmp(genre, "iTunesU"));
}

/* Each track's size is estimated from its bitrate and length: the database
 * holds no file sizes. */
static void count_library(struct about_info *info)
{
    struct tagcache_search tcs;
    char genre[32];
    if (!tagcache_search(&tcs, tag_genre))
        return;
    while (tagcache_get_next(&tcs, genre, sizeof(genre)))
    {
        if (tagcache_get_numeric(&tcs, tag_virt_spoken) == 2)
            info->podcasts++;
        else if (is_itunes_u_genre(genre))
            info->itunes_u++;
        else
            info->songs++;
        info->audio_kib +=
            (unsigned long long)tagcache_get_numeric(&tcs, tag_bitrate) *
            tagcache_get_numeric(&tcs, tag_length) / 8192;
    }
    tagcache_search_finish(&tcs);
}

/* Count and size indexed photos or videos from the published path list so
 * counts and capacity segments work across reboots as well as fresh scans. */
static void count_flat_media(enum flat_kind kind, int *count_out,
                             unsigned long long *kib_out)
{
    struct path_list pl;
    unsigned long long bytes = 0;
    int count = file_index_count(kind);

    if (path_list_load(&pl, file_index_list(kind), PATH_LIST_MAX))
    {
        if (pl.count > count)
            count = pl.count;
        for (int i = 0; i < pl.count; i++)
        {
            const char *line = path_list_get(&pl, i);
            const char *tab = strchr(line, '\t');
            const char *path = tab ? tab + 1 : line;
            int fd = open(path, O_RDONLY);
            if (fd >= 0)
            {
                off_t sz = ffilesize(fd);
                if (sz > 0)
                    bytes += (unsigned long long)sz;
                close(fd);
            }
            if ((i & 15) == 15)
                yield();
        }
        path_list_free(&pl);
    }
    *count_out = count;
    *kib_out = (bytes + 1023) / 1024;
}

static bool has_vcf_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".vcf");
}

static int count_vcards_in_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[512];
    char line[32];
    int line_len = 0, count = 0;
    bool line_full = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            char c = buf[i];
            if (c == '\r' || c == '\n')
            {
                line[line_len] = '\0';
                if (!strcasecmp(line, "BEGIN:VCARD"))
                    count++;
                line_len = 0;
                line_full = false;
            }
            else if (!line_full)
            {
                if (line_len == 0 && (c == ' ' || c == '\t'))
                    continue;
                if (line_len < (int)sizeof(line) - 1)
                    line[line_len++] = c;
                else
                    line_full = true;
            }
        }
    }
    if (line_len > 0)
    {
        line[line_len] = '\0';
        if (!strcasecmp(line, "BEGIN:VCARD"))
            count++;
    }
    close(fd);
    return count;
}

static int count_contacts(void)
{
    DIR *dir = opendir(CONTACTS_DIR);
    if (!dir)
        return 0;
    struct dirent *entry;
    char path[MAX_PATH];
    int total = 0;
    while ((entry = readdir(dir)))
    {
        const char *name = (const char *)entry->d_name;
        if (name[0] == '.' || !has_vcf_ext(name))
            continue;
        struct dirinfo di = dir_get_info(dir, entry);
        if (di.attribute & ATTR_DIRECTORY)
            continue;
        snprintf(path, sizeof(path), CONTACTS_DIR "/%s",
                 (const char *)entry->d_name);
        total += count_vcards_in_file(path);
        yield();
    }
    closedir(dir);
    return total;
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
    count_flat_media(FLAT_IMAGES, &info->photos, &info->photo_kib);
    count_flat_media(FLAT_VIDEOS, &info->videos, &info->video_kib);
    info->games = app_list_count(APP_LIST_GAMES);
    info->contacts = count_contacts();
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

/* Vertical gloss palettes for Video (purple) and Photos (amber/gold),
 * matching stock's Settings_About_Capacity_{Video,Photos}Legend swatches
 * across rows 4..21 of the 30-row capacity bar bitmap. */
static const uint32_t video_rows[18] = {
    0xb2b8f6, 0xafb5f4, 0xabb1f1, 0xa8aef0, 0xa4aaed,
    0xa0a6ea, 0x9ca2e7, 0x999fe4, 0x969ce2,
    0x7680e0, 0x747ddc, 0x717ad9, 0x6f77d5, 0x6c74d1,
    0x6a71ce, 0x686eca, 0x666cc7, 0x6a70cb,
};

static const uint32_t photos_rows[18] = {
    0xf4cb74, 0xf2c86e, 0xefc469, 0xecc064, 0xe9bc5e,
    0xe7b95a, 0xe4b555, 0xe1b251, 0xdeae4c,
    0xe5ab32, 0xe1a62f, 0xdda22d, 0xd99e2b, 0xd49a29,
    0xd09627, 0xcc9225, 0xc88e23, 0xce952a,
};

static inline uint32_t art_texel(const struct stock_art *art, int x, int y)
{
    int i = (y - art->ink_y) * art->ink_w + (x - art->ink_x);
    if (!art->colors)
        return (uint32_t)((const uint8_t *)art->pixels)[i] << 24;
    unsigned n = art->colors <= 256 ? ((const uint8_t *)art->pixels)[i] :
                                      ((const uint16_t *)art->pixels)[i];
    return art->palette[n];
}

/* Draw `id` (AudioLeft/Center/Right) recoloured to `palette` (Video or Photos)
 * while keeping the bitmap's exact rounded caps, bevel shading and shadows. */
static void put_bar_recolored(struct stock_canvas *c, enum stock_art_id id,
                              int sx, int width, int x,
                              const uint32_t *palette)
{
    if (!stock_art_ready())
        return;
    const struct stock_art *art = &stock_art[id];
    const struct stock_art *ref =
        &stock_art[STOCK_ART_CAPACITYVIEW_DETAILED_AUDIOCENTER];
    int h = art->height;
    int y0 = MAX(BAR_Y, c->y);
    int y1 = MIN(BAR_Y + h, c->y + c->height);
    int x0 = MAX(x, c->x);
    int x1 = MIN(x + width, c->x + c->width);
    for (int py = y0; py < y1; py++)
    {
        int sy = py - BAR_Y;
        uint32_t ref_t = art_texel(ref, 0, sy);
        int ref_lum = (((ref_t >> 16) & 255) * 77 + ((ref_t >> 8) & 255) * 150 +
                       (ref_t & 255) * 29) >> 8;
        uint32_t base = (sy >= 4 && sy <= 21) ? palette[sy - 4] : 0;
        for (int px = x0; px < x1; px++)
        {
            int ax = sx + (px - x);
            if (ax < art->ink_x || ax >= art->ink_x + art->ink_w ||
                sy < art->ink_y || sy >= art->ink_y + art->ink_h)
                continue;
            uint32_t t = art_texel(art, ax, sy);
            unsigned a = t >> 24;
            if (!a)
                continue;
            uint32_t rgb = t & 0xffffff;
            if (sy >= 4 && sy <= 21 && ref_lum > 0)
            {
                int lum = (((t >> 16) & 255) * 77 + ((t >> 8) & 255) * 150 +
                           (t & 255) * 29) >> 8;
                int scale = (lum * 256) / ref_lum;
                int r = MIN(255, (((int)(base >> 16) & 255) * scale) >> 8);
                int g = MIN(255, (((int)(base >> 8) & 255) * scale) >> 8);
                int b = MIN(255, (((int)(base & 255)) * scale) >> 8);
                rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
            stock_canvas_blend(c, px, py, rgb, a);
        }
    }
}

static void put_seg_piece(struct stock_canvas *c, enum bar_segment seg,
                          int piece, int sx, int width, int x)
{
    /* piece: 0 = left cap, 1 = 1px center column, 2 = right cap */
    static const enum stock_art_id audio_ids[3] = {
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIOLEFT,
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIOCENTER,
        STOCK_ART_CAPACITYVIEW_DETAILED_AUDIORIGHT,
    };
    static const enum stock_art_id other_ids[3] = {
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERLEFT,
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERCENTER,
        STOCK_ART_CAPACITYVIEW_DETAILED_OTHERRIGHT,
    };
    if (seg == SEG_AUDIO || seg == SEG_OTHER)
    {
        enum stock_art_id id = seg == SEG_AUDIO ? audio_ids[piece] :
                                                  other_ids[piece];
        stock_art_put_part(c, id, sx, 0, width, stock_art[id].height,
                           x, BAR_Y);
    }
    else
    {
        put_bar_recolored(c, audio_ids[piece], sx, width, x,
                          seg == SEG_VIDEO ? video_rows : photos_rows);
    }
}

static void put_stretched(struct stock_canvas *c, enum stock_art_id id,
                          int x0, int x1)
{
    for (int x = x0; x < x1; x++)
        stock_art_put(c, id, x, BAR_Y);
}

static void put_seg_stretched(struct stock_canvas *c, enum bar_segment seg,
                              int x0, int x1)
{
    for (int x = x0; x < x1; x++)
        put_seg_piece(c, seg, 1, 0, 1, x);
}

/* Audio, Video, Photos, then Other: each part's left cap shows only when it
 * is the first non-empty segment, and the next segment picks up from its
 * right edge. */
static void put_bar(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    unsigned long long used = info->total_kib - MIN(info->free_kib,
                                                    info->total_kib);
    unsigned long long audio = MIN(info->audio_kib, used);
    unsigned long long video = MIN(info->video_kib, used - audio);
    unsigned long long photo = MIN(info->photo_kib, used - audio - video);
    unsigned long long cum[SEG_COUNT] = {
        audio, audio + video, audio + video + photo, used,
    };
    int end[SEG_COUNT];
    for (int i = 0; i < SEG_COUNT; i++)
        end[i] = bar_column(cum[i]);

    int from = BAR_LEFT, last = -1;
    for (int i = 0; i < SEG_COUNT; i++)
    {
        int seg_end = end[i];
        if (seg_end <= from)
            continue;
        if (from < BAR_X + CAP)
        {
            int cap_start = last < 0 ? BAR_X : from;
            int cap_end = MIN(BAR_X + CAP, seg_end);
            put_seg_piece(c, i, 0, cap_start - BAR_X, cap_end - cap_start,
                          cap_start);
        }
        int mid_start = MAX(from, BAR_X + CAP);
        int mid_end = MIN(seg_end, RIGHT_CAP_X);
        if (mid_end > mid_start)
            put_seg_stretched(c, i, mid_start, mid_end);
        from = seg_end;
        last = i;
    }
    int used_end = end[SEG_OTHER];
    if (last < 0)
        stock_art_put(c, STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYLEFT, BAR_X,
                      BAR_Y);
    put_stretched(c, STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYCENTER,
                  MAX(used_end, BAR_X + CAP), RIGHT_CAP_X);
    bool full = last >= 0 && used_end >= BAR_RIGHT - CAP / 2;
    if (full)
        put_seg_piece(c, last, 2, 0, CAP, RIGHT_CAP_X);
    else
        stock_art_put(c, STOCK_ART_CAPACITYVIEW_DETAILED_EMPTYRIGHT,
                      RIGHT_CAP_X, BAR_Y);
    if (last >= 0 && !full)
        stock_art_put(c, STOCK_ART_CAPACITYVIEW_DETAILED_EDGE,
                      used_end - EDGE_W, BAR_Y);
}

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int top, uint32_t rgb)
{
    stock_canvas_text(c, font, text, x, top, rgb);
}

static void put_space(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    char amount[16], text[48];
    format_size(amount, sizeof(amount),
                info->total_kib - MIN(info->free_kib, info->total_kib));
    snprintf(text, sizeof(text), str(LANG_ABOUT_USED), amount);
    put_text(c, about.bold, text, USED_MID - text_width(text, about.bold) / 2,
             SPACE_TOP, WHITE);
    format_size(amount, sizeof(amount), info->free_kib);
    snprintf(text, sizeof(text), str(LANG_ABOUT_FREE), amount);
    put_text(c, about.bold, text, FREE_MID - text_width(text, about.bold) / 2,
             SPACE_TOP, WHITE);
}

static void put_legend(struct stock_canvas *c)
{
    static const struct {
        enum stock_art_id key;
        int label, x;
    } item[] = {
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_AUDIOLEGEND, LANG_ABOUT_AUDIO, 30 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_VIDEOLEGEND, LANG_ABOUT_VIDEO,
          110 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_PHOTOSLEGEND, LANG_ABOUT_PHOTOS,
          191 },
        { STOCK_ART_SETTINGS_ABOUT_CAPACITY_OTHERLEGEND, LANG_ABOUT_OTHER,
          270 },
    };
    for (int i = 0; i < (int)ARRAYLEN(item); i++)
    {
        stock_art_put(c, item[i].key, LEGEND_X + i * LEGEND_PITCH, LEGEND_Y);
        put_text(c, about.bold, str(item[i].label), item[i].x, LEGEND_Y, WHITE);
    }
}

/* The library's seven counts in stock 2.0.5's two columns: left column holds
 * Songs, Videos, Podcasts, iTunes U; right column holds Photos, Games,
 * Contacts. Each value is right-aligned against the column's right edge. */
static void put_counts(struct stock_canvas *c)
{
    const struct about_info *info = &about.info;
    const struct {
        int label, value, col, row;
    } cell[] = {
        { LANG_ABOUT_SONGS, info->songs, 0, 0 },
        { LANG_ABOUT_VIDEOS, info->videos, 0, 1 },
        { LANG_ABOUT_PODCASTS, info->podcasts, 0, 2 },
        { LANG_ABOUT_ITUNES_U, info->itunes_u, 0, 3 },
        { LANG_ABOUT_PHOTOS, info->photos, 1, 0 },
        { LANG_ABOUT_GAMES, info->games, 1, 1 },
        { LANG_ABOUT_CONTACTS, info->contacts, 1, 2 },
    };
    static const int label_x[] = { 19, 174 }, value_right[] = { 153, 306 };
    for (int i = 0; i < (int)ARRAYLEN(cell); i++)
    {
        int top = COUNTS_TOP + cell[i].row * ROW_PITCH;
        char value[12];
        snprintf(value, sizeof(value), "%d", cell[i].value);
        put_text(c, about.bold, str(cell[i].label), label_x[cell[i].col], top,
                 WHITE);
        put_text(c, about.regular, value,
                 value_right[cell[i].col] - text_width(value, about.regular),
                 top, VALUE_GREY);
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
        int top = IDENTITY_TOP + i * ROW_PITCH;
        put_text(c, about.bold, str(row[i].label), 18, top, WHITE);
        put_text(c, about.regular, row[i].value, 192, top, VALUE_GREY);
    }
}

static void put_page(struct stock_canvas *c, int page, int offset)
{
    /* Translate the content over the stationary background. The canvas
     * dimensions clip it at the panel edges. */
    struct stock_canvas shifted = *c;
    shifted.x -= offset;
    if (page == 0)
    {
        put_bar(&shifted);
        put_legend(&shifted);
    }
    else if (page == 1)
        put_counts(&shifted);
    else
        put_identity(&shifted);
}

static void compose(struct stock_canvas *c, int from, int incoming)
{
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    if (c->y < BAR_Y)
    {
        stock_art_put(c, STOCK_ART_SETTINGS_ABOUT_BAND, 0, BAND_Y);
        stock_art_put(c, STOCK_ART_ASTRAPOD_ABOUT_LOGO, LOGO_X, LOGO_Y);
        put_space(c);
    }
    if (c->y + c->height > BAR_Y)
    {
        if (incoming)
            put_page(c, from, incoming - LCD_WIDTH);
        put_page(c, about.page, incoming);
    }
    for (int i = 0; i < PAGES; i++)
        stock_art_put(c, i == about.page ?
                      STOCK_ART_SETTINGS_ABOUT_BUBBLE_SELECTED :
                      STOCK_ART_SETTINGS_ABOUT_BUBBLE_DESELECTED,
                      BUBBLE_X + i * BUBBLE_PITCH, BUBBLE_Y);
    extras_bar_put_ex(c, str(LANG_ROCKBOX_TITLE), true, true);
}

/* Composed at 8 bits a channel, as stock composes, in as many rows at a time
 * as the scratch buffer holds, and truncated once. */
static void paint(int y0, int y1, int from, int incoming)
{
    static const char owner[] = "About";
    size_t size;
    uint32_t *pixels = app_try_claim_buffer(&size, owner);
    if (!pixels)
        return;
    int rows = MAX(1, (int)(size / (LCD_WIDTH * sizeof(uint32_t))));
    struct ui_rect clip = { 0, y0, LCD_WIDTH, y1 - y0 };
    for (int y = y0; y < y1; y += rows)
    {
        struct stock_canvas band = {
            pixels, 0, y, LCD_WIDTH, MIN(rows, y1 - y),
        };
        compose(&band, from, incoming);
        stock_canvas_show(&band, clip);
    }
    app_release_buffer(owner);
    lcd_update_rect(0, y0, LCD_WIDTH, y1 - y0);
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
            LANG_ABOUT_ITUNES_U, LANG_ABOUT_PHOTOS, LANG_ABOUT_GAMES,
            LANG_ABOUT_CONTACTS,
        };
        const int value[] = {
            info->songs, info->videos, info->podcasts, info->itunes_u,
            info->photos, info->games, info->contacts,
        };
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

/* PLAY maps locally in themes without global wheel transport. */
static const struct button_mapping *about_keymap(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_STD_OK, BUTTON_SELECT | BUTTON_REL, BUTTON_SELECT },
        { ACTION_STD_CANCEL, BUTTON_MENU | BUTTON_REL, BUTTON_MENU },
        { ACTION_WPS_PLAY, BUTTON_PLAY | BUTTON_REL, BUTTON_PLAY },
        { ACTION_NONE, BUTTON_PLAY | BUTTON_REPEAT, BUTTON_NONE },
        { ACTION_STD_NEXT, BUTTON_RIGHT | BUTTON_REL, BUTTON_RIGHT },
        { ACTION_STD_PREV, BUTTON_LEFT | BUTTON_REL, BUTTON_LEFT },
        LAST_ITEM_IN_LIST
    };
    return keys;
}

/* Stock moves only the contents, always to the left. Its eight steps
 * advance every ~34 ms with a quadratic ease-out. */
static int turn_offset(unsigned frame)
{
    unsigned left = TURN_FRAMES - frame;
    return LCD_WIDTH * left * left / (TURN_FRAMES * TURN_FRAMES);
}

static long turn_tick(long start, unsigned frame)
{
    return start + (frame * TURN_FRAME_MS * HZ + 500) / 1000;
}

static unsigned bar_stamp(void)
{
    return (unsigned)(battery_level() & 0xff) |
           ((unsigned)(charge_state & 0x0f) << 8) |
           ((unsigned)(current_playmode() & 0x0f) << 12) |
           ((unsigned)(button_hold() ? 1 : 0) << 16) |
           ((unsigned)(charger_inserted() ? 1 : 0) << 17);
}

int fruit_about_screen(void)
{
    int ret = 0;
    struct ui_scene scene = {0};

    ui_scene_open_page(&scene, "fruit-about-push");
    about.page = 0;
    about.bold = load_font("13-helvetica-bold");
    about.regular = load_font("13-helvetica");
    read_info(&about.info);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    paint(0, LCD_HEIGHT, 0, 0);
    speak_page();
    unsigned last_bar = bar_stamp();
    struct ui_motion turn = {0};
    unsigned frame = TURN_FRAMES;
    int from = 0;
    long start = 0;

    while (1)
    {
        int timeout = HZ / 2;
        if (frame < TURN_FRAMES)
            timeout = MAX(0, turn_tick(start, frame + 1) - current_tick);
        int action = get_custom_action(
            CONTEXT_PLUGIN, timeout, about_keymap);
        if (action == ACTION_STD_CANCEL)
            break;

        if (action == ACTION_STD_OK || action == ACTION_STD_NEXT ||
            action == ACTION_STD_PREV)
        {
            from = about.page;
            int step = action == ACTION_STD_PREV ? -1 : 1;
            about.page = (from + step + PAGES) % PAGES;
            frame = global_settings.animations ? 0 : TURN_FRAMES;
            if (frame == 0)
            {
                if (turn.lease || turn.trace)
                    ui_motion_renew(&turn);
                else
                    turn = ui_motion_begin("fruit-about-slide");
            }
            paint(BAR_Y, LCD_HEIGHT, from, turn_offset(frame));
            if (frame == TURN_FRAMES)
                ui_motion_end(&turn);
            /* The first paint also loads the incoming page's glyphs. */
            start = current_tick;
            speak_page();
        }
        else if (action == ACTION_WPS_PLAY)
            wps_wheel_transport(action);
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = MENU_ATTACHED_USB;
            break;
        }

        unsigned next = frame;
        while (next < TURN_FRAMES &&
               TIME_AFTER(current_tick, turn_tick(start, next + 1) - 1))
            next++;
        if (next != frame)
        {
            frame = next;
            paint(BAR_Y, LCD_HEIGHT, from, turn_offset(frame));
            if (frame == TURN_FRAMES)
                ui_motion_end(&turn);
        }

        unsigned now_bar = bar_stamp();
        if (now_bar != last_bar)
        {
            last_bar = now_bar;
            paint(0, EXTRAS_BAR_HEIGHT, from, turn_offset(frame));
        }
    }

    ui_motion_end(&turn);
    talk_shutup();
    extras_bar_release();
    ui_scene_close_modal(&scene, ret == 0, "fruit-about-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (about.regular != FONT_UI)
        font_unload(about.regular);
    if (about.bold != FONT_UI)
        font_unload(about.bold);
    return ret;
}
