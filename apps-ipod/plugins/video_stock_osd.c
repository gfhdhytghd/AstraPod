/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock iPod Classic 2.0.5 movie overlay, for Classic and Fruit Company.
 *
 * Stock draws its controls on a UI layer that the LCD controller blends over
 * the movie: a top band (rows 0-23: title, play state, battery) and a bottom
 * band (rows 203-239: one page of controls). `layer` is that UI layer,
 * ARGB8888, composed only when what it shows changes. Each frame converts the
 * movie rows under the bands from YUV exactly as lcd_blit_yuv() does, blends
 * the layer over them as the controller does and sends those rows from the
 * framebuffer; lcd_blit_yuv() sends the rows between.
 *
 * Parts: composing the layer (over, put, tile, text), the pages, the slides,
 * and presenting a frame.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "bidi.h"
#include "backlight.h"
#include "core_alloc.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "power.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "ui/stock_art.h"
#include "ui/theme_art.h"
#include "video_stock_osd.h"

#define TOP_ROWS     24
#define BOTTOM_Y     203
#define BOTTOM_ROWS  37
#define LAYER_ROWS   (TOP_ROWS + BOTTOM_ROWS)
#define PICTURE_Y    TOP_ROWS
#define BAND_Y       202  /* the bottom band from an even row: YUV pairs */
#define SLIDE_ROWS   37   /* how far both bands slide to show or hide */
#define SLIDE_TICKS  (HZ / 10)
#define PAGE_TICKS   (HZ / 4)

#define TITLE_FONT   FONT_DIR "/13-helvetica-bold.pbf"
#define TIME_FONT    FONT_DIR "/14-helvetica-bold.pbf"
#define TITLE_GREY   0xd2d2d2u
#define TITLE_WIDTH  216
#define WHITE        0xffffffu

/* Bars: the progress bar and the level bars of Volume and Brightness. */
#define BAR_Y        212
#define PROGRESS_X   54
#define PROGRESS_W   212
#define LEVEL_W      208
#define TIME_RIGHT   44   /* elapsed time ends here */
#define TIME_LEFT    276  /* remaining time starts here */
#define TIME_BASE    223

static int layer_handle;
static uint32_t *layer;
static int title_font = -1, time_font = -1;
static bool brightness_changed;

/* --- Composing the layer ------------------------------------------------ */

/* The layer row under screen row `y`, or NULL between the bands. */
static uint32_t *layer_row(int y)
{
    if (y >= 0 && y < TOP_ROWS)
        return layer + y * LCD_WIDTH;
    if (y >= BOTTOM_Y && y < LCD_HEIGHT)
        return layer + (y - BOTTOM_Y + TOP_ROWS) * LCD_WIDTH;
    return NULL;
}

/* How a source pixel's colour joins the layer's. Stock's UI layer keeps
 * straight alpha and copies a source over a clear pixel; elsewhere these are
 * the rules that fit the 2.0.5 captures, by what is drawn. */
enum rule {
    ICON,  /* d + (s - d) * sa / 255 */
    TEXT,  /* the mean of ICON and the Porter-Duff colour; glyphs and the
            * level bars' end icons */
    BAR,   /* Porter-Duff, divided by oa + 0.3 * (255 - oa) */
};

static inline void over(uint32_t *d, uint32_t s, enum rule rule)
{
    unsigned sa = s >> 24, da = *d >> 24;
    if (!sa)
        return;
    if (!da || sa == 255)
    {
        *d = s;
        return;
    }
    unsigned oa = sa + da * (255 - sa) / 255;
    uint32_t out = (uint32_t)oa << 24;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        unsigned sc = (s >> shift) & 255, dc = (*d >> shift) & 255;
        unsigned pd = sc * sa + dc * da * (255 - sa) / 255;
        unsigned mix = (sc * sa + dc * (255 - sa)) / 255, c;
        if (rule == ICON)
            c = mix;
        else if (rule == TEXT)
            c = (pd / oa + mix) >> 1;
        else
            c = pd / (oa + 3 * (255 - oa) / 10);
        out |= (uint32_t)c << shift;
    }
    *d = out;
}

/* Columns sx to sx + width of image `id` with their left edge at screen
 * (x, y), drawn only between columns clip0 and clip1. */
static void put_part(enum stock_art_id id, int sx, int width, int x, int y,
                     int clip0, int clip1, enum rule rule)
{
    const struct stock_art *art = &stock_art[id];
    clip0 = MAX(clip0, 0);
    clip1 = MIN(clip1, LCD_WIDTH);
    for (int row = 0; row < art->height; row++)
    {
        uint32_t *out = layer_row(y + row);
        if (!out)
            continue;
        for (int col = 0; col < width; col++)
        {
            int px = x + col, i = row * art->width + sx + col;
            if (px < clip0 || px >= clip1)
                continue;
            unsigned n = art->colors <= 256 ?
                ((const uint8_t *)art->pixels)[i] :
                ((const uint16_t *)art->pixels)[i];
            over(&out[px], art->palette[n], rule);
        }
    }
}

static void put(enum stock_art_id id, int x, int y, enum rule rule)
{
    put_part(id, 0, stock_art[id].width, x, y, 0, LCD_WIDTH, rule);
}

/* Repeat `id` across `width` columns from x. A last part tile shows the
 * image's right-hand columns, so the bar always ends on its end cap. */
static void tile(enum stock_art_id id, int x, int y, int width)
{
    int w = stock_art[id].width, i = 0;
    for (; i + w <= width; i += w)
        put_part(id, 0, w, x + i, y, 0, LCD_WIDTH, BAR);
    if (i < width)
        put_part(id, w - (width - i), width - i, x + i, y, 0, LCD_WIDTH,
                 BAR);
}

static int text_width(int font, const char *text)
{
    int width = 0;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    return width;
}

/* `text` with its baseline at screen row `baseline`, each glyph's coverage
 * the alpha of `rgb`. */
static void text(int font, const char *text, int x, int baseline,
                 uint32_t rgb)
{
    struct font *pf = font_get(font);
    struct font_run run = { 0 };
    int top = baseline - pf->ascent;
    font_lock(font, true);
    for (const ucschar_t *ucs = bidi_l2v((const unsigned char *)text, 1);
         pf->pbf.file_size && ucs && *ucs; ucs++)
    {
        struct font_glyph g;
        font_run_next(pf, &run, *ucs, &g);
        if (!g.bits)
            continue;
        for (int row = 0; row < g.height; row++)
        {
            uint32_t *out = layer_row(top + g.y + row);
            if (!out)
                continue;
            for (int col = 0; col < g.width; col++)
            {
                int px = x + g.x + col, i = row * g.width + col;
                unsigned a = g.coverage8 ? g.bits[i] :
                    (15 - ((g.bits[i >> 1] >> (i & 1 ? 4 : 0)) & 15)) * 17;
                if (a && px >= 0 && px < LCD_WIDTH)
                    over(&out[px], rgb | (uint32_t)a << 24, TEXT);
            }
        }
    }
    font_lock(font, false);
}

/* --- The pages ------------------------------------------------------------ */

/* What one page of the bottom band shows. */
struct page {
    unsigned char mode;
    int elapsed, remaining;  /* seconds */
    int marker;              /* scrub marker's centre on the bar, or -1 */
    int fill;                /* level bar's blue columns */
    const char *label;
};

static struct page page_of(const struct video_stock_state *s)
{
    struct page p;
    memset(&p, 0, sizeof(p));  /* compared with memcmp(), padding too */
    p.mode = s->mode;
    p.marker = -1;
    p.label = s->picture;
    uint32_t duration = s->duration_ms;
    uint32_t position = MIN(s->position_ms, duration);
    if (s->mode == VIDEO_STOCK_PROGRESS || s->mode == VIDEO_STOCK_SCRUB)
    {
        p.elapsed = position / 1000;
        p.remaining = (duration - position) / 1000;
        if (s->mode == VIDEO_STOCK_SCRUB && duration)
            p.marker = (int)((uint64_t)position * PROGRESS_W / duration);
    }
    else if (s->level_max > 0)
        p.fill = MIN(MAX(s->level, 0), s->level_max) * LEVEL_W / s->level_max;
    return p;
}

static void time_text(char *buffer, size_t size, const char *sign, int s)
{
    if (s >= 3600)
        snprintf(buffer, size, "%s%d:%02d:%02d", sign, s / 3600, s / 60 % 60,
                 s % 60);
    else
        snprintf(buffer, size, "%s%d:%02d", sign, s / 60, s % 60);
}

static void level_bar(int x, int fill)
{
    tile(STOCK_ART_VIDEO_PROGRESSBAR_FILL, x, BAR_Y, fill);
    tile(STOCK_ART_VIDEO_PROGRESSBAR, x + fill, BAR_Y, LEVEL_W - fill);
}

/* The page's controls with everything moved `dx` columns. */
static void put_page(const struct page *p, int dx)
{
    char buffer[16];
    switch (p->mode)
    {
    case VIDEO_STOCK_PROGRESS:
    case VIDEO_STOCK_SCRUB:
        tile(STOCK_ART_VIDEO_PROGRESSBAR, PROGRESS_X + dx, BAR_Y, PROGRESS_W);
        if (p->marker >= 0)
        {
            /* The diamond is cut to the bar: a triangle at either end. */
            enum stock_art_id id = STOCK_ART_NOWPLAYING_PROGRESSBAR_SCRUB;
            int w = stock_art[id].width;
            put_part(id, 0, w, PROGRESS_X + dx + p->marker - w / 2, BAR_Y,
                     PROGRESS_X + dx, PROGRESS_X + dx + PROGRESS_W, BAR);
        }
        time_text(buffer, sizeof(buffer), "", p->elapsed);
        text(time_font, buffer, TIME_RIGHT + dx - text_width(time_font, buffer),
             TIME_BASE, WHITE);
        time_text(buffer, sizeof(buffer), "-", p->remaining);
        text(time_font, buffer, TIME_LEFT + dx, TIME_BASE, WHITE);
        break;
    case VIDEO_STOCK_VOLUME:
        put(STOCK_ART_VIDEO_VOLUME_LOW, 31 + dx, 212, TEXT);
        put(STOCK_ART_VIDEO_VOLUME_HIGH, 274 + dx, 210, TEXT);
        level_bar(56 + dx, p->fill);
        break;
    case VIDEO_STOCK_BRIGHTNESS:
        put(STOCK_ART_VIDEO_BRIGHTNESS_LOW, 36 + dx, 212, TEXT);
        put(STOCK_ART_VIDEO_BRIGHTNESS_HIGH, 276 + dx, 212, TEXT);
        level_bar(58 + dx, p->fill);
        break;
    case VIDEO_STOCK_PICTURE:
        if (p->label)
            text(time_font, p->label,
                 (LCD_WIDTH - text_width(time_font, p->label) + 1) / 2 + dx,
                 TIME_BASE, WHITE);
        break;
    }
}

static void compose_bottom(const struct page *now, const struct page *old,
                           int dx)
{
    int step = stock_art[STOCK_ART_VIDEO_OVERLAY].width;
    memset(layer_row(BOTTOM_Y), 0, BOTTOM_ROWS * LCD_WIDTH * sizeof(*layer));
    for (int x = 0; x < LCD_WIDTH; x += step)
        put(STOCK_ART_VIDEO_OVERLAY, x, BOTTOM_Y, ICON);
    if (old)
        put_page(old, dx - LCD_WIDTH);
    put_page(now, dx);
}

static const enum stock_art_id battery_art[] = {
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_0,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_1,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_2,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_3,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_4,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_5,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_6,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_7,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_8,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_9,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_10,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_11,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_12,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_13,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_14,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_15,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_16,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_17,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_18,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_19,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_20,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_21,
    STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_22,
};

/* What the top band shows; composed again when any of it changes. */
struct top {
    const char *title;
    bool paused;
    int battery;  /* battery_art index, -1 without a level */
    int charge;   /* 0, plug or charging */
};

static struct top top_now(const struct video_stock_state *s)
{
    struct top t;
    memset(&t, 0, sizeof(t));
    t.title = s->title;
    t.paused = s->paused;
    t.battery = -1;
    int level = battery_level();
    if (level >= 0)
        t.battery = MIN((int)ui_art_battery_index(level),
                        (int)ARRAYLEN(battery_art) - 1);
    if (charge_state == CHARGING || charge_state == TOPOFF)
        t.charge = 2;
#if CONFIG_CHARGING
    else if (charger_inserted())
        t.charge = 1;
#endif
    return t;
}

static void compose_top(const struct top *t)
{
    memset(layer, 0, TOP_ROWS * LCD_WIDTH * sizeof(*layer));
    put(STOCK_ART_VIDEO_STATUSBAR, 0, 0, ICON);
    if (t->title && *t->title)
    {
        char title[MAX_PATH + 4];
        strlcpy(title, t->title, MAX_PATH);
        size_t len = strlen(title);
        while (len > 1 && text_width(title_font, title) > TITLE_WIDTH)
        {
            /* Drop a whole UTF-8 character and end with an ellipsis. */
            do
                len--;
            while (len > 1 && (title[len] & 0xc0) == 0x80);
            strcpy(title + len, "\xe2\x80\xa6");
        }
        text(title_font, title,
             (LCD_WIDTH - text_width(title_font, title) + 1) / 2, 15,
             TITLE_GREY);
    }
    put(t->paused ? STOCK_ART_VIDEO_PAUSE : STOCK_ART_VIDEO_PLAY, 270, 3, ICON);
    if (t->battery >= 0)
        put(battery_art[t->battery], 289, 4, ICON);
    if (t->charge)
        put(t->charge == 2 ? STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_CHARGE :
                             STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_PLUG,
            289, 4, ICON);
}

/* --- The slides ----------------------------------------------------------- */

/* Both bands slide off by the same rows, the top band up and the bottom
 * band down: in by an ease-out, out by an ease-in, in SLIDE_TICKS. */
struct band {
    bool shown;
    long start;
};

static struct band bands[2];  /* top, bottom */
static struct page page_new, page_old;
static long page_start;
static bool paging;
static struct top top_shown;
static struct page bottom_shown, bottom_old;
static int bottom_dx;
static bool top_ready, bottom_ready;

static int band_offset(const struct band *b, long now)
{
    int t = MIN(MAX(now - b->start, 0), SLIDE_TICKS);
    int rest = SLIDE_TICKS - t;
    return b->shown ? SLIDE_ROWS * rest * rest / (SLIDE_TICKS * SLIDE_TICKS) :
                      SLIDE_ROWS * t * t / (SLIDE_TICKS * SLIDE_TICKS);
}

/* Turn a band round from wherever it is: the new slide starts at the
 * offset the old one had reached. */
static void band_set(struct band *b, bool shown, long now)
{
    if (b->shown == shown)
        return;
    int offset = band_offset(b, now);
    b->shown = shown;
    b->start = now;
    while (b->start > now - SLIDE_TICKS &&
           (shown ? band_offset(b, now) > offset :
                    band_offset(b, now) < offset))
        b->start--;
}

static int page_dx(long now)
{
    int t = MIN(now - page_start, PAGE_TICKS), rest = PAGE_TICKS - t;
    return LCD_WIDTH * rest * rest / (PAGE_TICKS * PAGE_TICKS);
}

/* --- Presenting ----------------------------------------------------------- */

static bool prepare(void)
{
    if (!stock_art_ready())
        return false;
    if (!layer)
    {
        layer_handle = core_alloc(LAYER_ROWS * LCD_WIDTH * sizeof(*layer));
        if (layer_handle <= 0)
            return false;
        layer = core_get_data_pinned(layer_handle);
        top_ready = bottom_ready = false;
    }
    if (title_font < 0)
        title_font = font_load_ex(TITLE_FONT, 0,
                                  global_settings.glyphs_to_cache);
    if (time_font < 0)
        time_font = font_load_ex(TIME_FONT, 0,
                                 global_settings.glyphs_to_cache);
    return title_font >= 0 && time_font >= 0;
}

static inline unsigned div255(unsigned n)
{
    return ((n + 1) * 257) >> 16;
}

static inline int clamp(int v, int top)
{
    return v < 0 ? 0 : v > top ? top : v;
}

/* Rows y to y + rows of the picture, y and rows even, into the framebuffer
 * with the layer over them, the band slid `offset` rows up (top) or down. */
static void band_rows(unsigned char *const planes[3], int y, int rows,
                      int offset, bool top)
{
    for (int r = y; r < y + rows; r++)
    {
        const uint8_t *py = planes[0] + r * LCD_WIDTH;
        const uint8_t *pu = planes[1] + (r >> 1) * (LCD_WIDTH / 2);
        const uint8_t *pv = planes[2] + (r >> 1) * (LCD_WIDTH / 2);
        int ly = top ? r + offset : r - offset;
        const uint32_t *in = (top ? ly < TOP_ROWS :
                              ly >= BOTTOM_Y && ly < LCD_HEIGHT) ?
                             layer_row(ly) : NULL;
        fb_data *out = FBADDR(0, r);
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            /* lcd_write_yuv420_lines(), step for step */
            int cb = pu[x >> 1] - 128, cr = pv[x >> 1] - 128;
            int luma = (py[x] - 16) * 37;
            int red = clamp((luma >> 8) + ((cr * 101 + 256) >> 9), 31);
            int green = clamp((luma >> 7) + ((128 - (cr * 51 + cb * 24)) >> 8),
                              63);
            int blue = clamp((luma >> 8) + ((cb + 2) >> 2), 31);
            uint32_t s = in ? in[x] : 0;
            unsigned a = s >> 24;
            if (a == 255)
            {
                red = (s >> 19) & 31;
                green = (s >> 10) & 63;
                blue = (s >> 3) & 31;
            }
            else if (a)
            {
                /* The controller widens the picture with zero low bits. */
                red = div255(((s >> 16) & 255) * a +
                             (red << 3) * (255 - a)) >> 3;
                green = div255(((s >> 8) & 255) * a +
                               (green << 2) * (255 - a)) >> 2;
                blue = div255((s & 255) * a + (blue << 3) * (255 - a)) >> 3;
            }
            out[x] = (red << 11) | (green << 5) | blue;
        }
    }
}

static bool present(unsigned char *const planes[3],
                    const struct video_stock_state *s)
{
    long now = current_tick;
    bool bottom = s->mode != VIDEO_STOCK_HIDDEN;

    if (bottom)
    {
        struct page p = page_of(s);
        if (bands[1].shown && p.mode != page_new.mode)
        {
            page_old = page_new;
            page_start = now;
            paging = true;
        }
        page_new = p;
    }
    band_set(&bands[0], bottom || s->paused, now);
    band_set(&bands[1], bottom, now);
    if (paging && now - page_start >= PAGE_TICKS)
        paging = false;

    int top_off = band_offset(&bands[0], now);
    int bottom_off = band_offset(&bands[1], now);
    bool top_on = top_off < TOP_ROWS, bottom_on = bottom_off < BOTTOM_ROWS;
    bool moving = (top_off && bands[0].shown) || (bottom_off && bands[1].shown)
                  || (top_on && !bands[0].shown)
                  || (bottom_on && !bands[1].shown) || paging;

    if ((!top_on && !bottom_on) || !prepare())
    {
        lcd_blit_yuv(planes, 0, 0, LCD_WIDTH, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        return moving;
    }

    if (top_on)
    {
        struct top t = top_now(s);
        if (!top_ready || memcmp(&t, &top_shown, sizeof(t)))
        {
            compose_top(&t);
            top_shown = t;
            top_ready = true;
        }
    }
    if (bottom_on)
    {
        int dx = paging ? page_dx(now) : 0;
        if (!bottom_ready || dx != bottom_dx ||
            memcmp(&page_new, &bottom_shown, sizeof(page_new)) ||
            (paging && memcmp(&page_old, &bottom_old, sizeof(page_old))))
        {
            compose_bottom(&page_new, paging ? &page_old : NULL, dx);
            bottom_shown = page_new;
            bottom_old = page_old;
            bottom_dx = dx;
            bottom_ready = true;
        }
    }

    int y0 = top_on ? PICTURE_Y : 0, y1 = bottom_on ? BAND_Y : LCD_HEIGHT;
    lcd_blit_yuv(planes, 0, y0, LCD_WIDTH, 0, y0, LCD_WIDTH, y1 - y0);
    if (top_on)
    {
        band_rows(planes, 0, TOP_ROWS, top_off, true);
        lcd_update_rect(0, 0, LCD_WIDTH, TOP_ROWS);
    }
    if (bottom_on)
    {
        band_rows(planes, BAND_Y, LCD_HEIGHT - BAND_Y, bottom_off, false);
        lcd_update_rect(0, BAND_Y, LCD_WIDTH, LCD_HEIGHT - BAND_Y);
    }
    return moving;
}

static int brightness(int step)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    int level = MIN(MAX(global_settings.brightness + step,
                        MIN_BRIGHTNESS_SETTING), MAX_BRIGHTNESS_SETTING);
    if (level != global_settings.brightness)
    {
        global_settings.brightness = level;
        backlight_set_brightness(level);
        brightness_changed = true;
    }
    return level - MIN_BRIGHTNESS_SETTING;
#else
    (void)step;
    return 0;
#endif
}

const struct video_stock_osd video_stock_osd = {
    .present = present,
    .brightness = brightness,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    .brightness_max = MAX_BRIGHTNESS_SETTING - MIN_BRIGHTNESS_SETTING,
#endif
};

void video_stock_osd_release(void)
{
    if (layer)
    {
        core_put_data_pinned(layer);
        layer_handle = core_free(layer_handle);
        layer = NULL;
    }
    if (title_font >= 0)
        font_unload(title_font);
    if (time_font >= 0)
        font_unload(time_font);
    title_font = time_font = -1;
    memset(bands, 0, sizeof(bands));
    memset(&page_new, 0, sizeof(page_new));
    paging = top_ready = bottom_ready = false;
    if (brightness_changed)
        settings_save();
    brightness_changed = false;
}
