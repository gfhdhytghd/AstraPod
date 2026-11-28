/***************************************************************************
 * PodBox split-screen menu pane. GPL version 2+.
 *
 * The right half of a menu under style.menu_pane=split, as the stock iPod
 * Classic draws it, from its own bitmaps: a slideshow of the library's
 * covers, or on the blue-grey backdrop the playing track, the player's free
 * space, or the selected row's name, picture and value. The rows cast a
 * shadow on its first columns.
 *
 * Parts: the page on screen and its depth; the fonts; the slideshow, whose
 * covers the background thread reads into one buffer; the painters, which
 * compose a still pane at 8 bits a channel as stock does; the content shown
 * and the pause before a new row's; and the motion.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "storage.h"
#include "system.h"
#include "audio.h"
#include "metadata.h"
#include "database/tagcache.h"
#include "lang.h"
#include "mv.h"
#include "settings/settings.h"
#include "draw/line.h"
#include "draw/viewport.h"
#include "metadata/album_palette.h"
#include "metadata/art_cache.h"
#include "system/bg_task.h"
#include "string-extra.h"
#include "animation.h"
#include "menu_pane.h"
#include "retained.h"
#include "native_theme_runtime.h"
#include "stock_art.h"
#include "ultra_pane.h"

#define PANE_W (LCD_WIDTH - MENU_PANE_X)
#define PANE_MID (MENU_PANE_X + PANE_W / 2)
/* PreviewArea_Shadow_Image's width. */
#define SHADOW_W 26
/* A cover is read 272 pixels square, so the pane has room to pan over it. */
#define SHOW_EDGE 272
#define SHOW_BYTES (SHOW_EDGE * SHOW_EDGE * sizeof(fb_data))
#define SHOW_EVENT 0x1185
#define SHOW_TRIES 8
#define PAN_X_RANGE (SHOW_EDGE - PANE_W)
#define PAN_Y_RANGE (SHOW_EDGE - LCD_HEIGHT)
/* Each moving axis pans 7.4 px/s, and a cover stays 6.8 s. */
#define PAN_US_PER_PX 135135u
#define SHOW_LIFE_US 6800000u
#define FADE_US 500000u
/* A disk that is asleep is woken for a new cover only this late. */
#define IDLE_DWELL_US 30000000u
#define CHANGE_DELAY_US 515000u
#define SLIDE_US 270000u
#define FRAME_US 33333u
#define PLAYING_POLL HZ
#define WRAP_W 132
/* Where stock draws its pictures. */
#define PREVIEW_X 178
#define PREVIEW_Y 44
#define PREVIEW_EDGE 119
#define TILE_X 202
#define TILE_Y 81
#define LOGO_X 202
#define LOGO_Y 65
#define SHUFFLE_X 180
#define SHUFFLE_Y 55
#define CAPACITY_X 179
#define CAPACITY_Y 173
#define CAPACITY_W 130
#define WHITE 0xffffff
#define TILE_GLYPH 0xeeeeee

enum show_state { SHOW_EMPTY, SHOW_REQUESTED, SHOW_LOADING, SHOW_FRESH,
                  SHOW_READY, SHOW_NONE };

static struct {
    int depth;
    bool split, on_screen;
} page = { -1, false, false };

static struct {
    unsigned generation;
    int title, text;
} fonts = { (unsigned)-1, -1, -1 };

/* The slideshow. Its buffer is kept while the theme splits menus, so a cover
 * stays in place from one menu to the next. The background thread owns it
 * from SHOW_LOADING until it posts SHOW_FRESH. */
static struct {
    int handle;
    volatile int state;
    volatile bool release;
    int album;              /* the cover in the buffer, or -1 */
    uint32_t started;       /* when the current cover began its pan */
    uint32_t fade_started;
    int fade_level;         /* how much of the cover the pane shows, of 32 */
    int x0, y0;             /* the window over the cover when it began */
    int dx, dy;             /* the window's step per pan tick */
    int drawn_x, drawn_y;
    uint32_t drawn_at, held_at;
    bool held, held_ready, keep_pixels;
    bool fading;
    uint32_t none_at;       /* when a load last ended without a cover */
} show = { .album = -1 };

static struct menu_pane_content shown, pending, under;
static struct menu_pane_content picture;
static unsigned identity;
static bool has_pending;
static uint32_t pending_at, slide_started, stepped_at;
/* 1 while the cover leaves to the right, -1 while it arrives from there,
 * with `under` the pane beneath it. */
static int slide_dir;
static char playing_key[300];
static long playing_polled;

static struct {
    unsigned requests, alloc_failures, work, yielded;
    unsigned no_key, no_file, fallback, open_failures, decode_failures, loads;
    unsigned cover_paints, fallback_paints, steps, held_steps;
    size_t available, allocatable;
} probe;

/* A still pane is composed a band of rows at a time: the whole pane at 8
 * bits a channel would take 150 KiB the firmware image has no room for. */
#define BAND_ROWS 16
static uint32_t band_pixels[PANE_W * BAND_ROWS];

/* ---- the page on screen ---- */

bool menu_pane_themed(void)
{
    return ui_theme_style(UI_STYLE_MENU_PANE) == UI_MENU_PANE_SPLIT;
}

static void show_free(void)
{
    if (show.state == SHOW_LOADING || show.state == SHOW_REQUESTED)
    {
        show.release = true;
        return;
    }
    if (show.handle > 0)
        show.handle = core_free(show.handle);
    show.state = SHOW_EMPTY;
    show.album = -1;
    show.keep_pixels = false;
}

void menu_pane_page(bool split)
{
    page.split = page.on_screen = split;
    page.depth = viewportmanager_depth(SCREEN_MAIN);
    if (!split && !show.held)
        show.keep_pixels = false;
    if (!menu_pane_themed())
        show_free();
}

int menu_pane_split(void)
{
    return page.split && page.depth == viewportmanager_depth(SCREEN_MAIN) &&
           menu_pane_themed() ? MENU_PANE_X : 0;
}

void menu_pane_viewport_left(int depth)
{
    if (depth >= page.depth)
        return;
    page.split = false;
    page.depth = -1;
}

bool menu_pane_take_screen(void)
{
    bool split = page.on_screen;
    page.on_screen = false;
    return split;
}

struct ui_rect menu_pane_rect(void)
{
    return (struct ui_rect){ MENU_PANE_X, 0, PANE_W, LCD_HEIGHT };
}

/* ---- fonts ---- */

static void load_fonts(void)
{
    unsigned generation = ui_theme_generation();
    if (fonts.generation == generation)
        return;
    font_unload(fonts.title);
    font_unload(fonts.text);
    int glyphs = global_settings.glyphs_to_cache;
    fonts.title = ui_theme_current_font_load(UI_FONT_PANE_TITLE, glyphs);
    fonts.text = ui_theme_current_font_load(UI_FONT_PANE_TEXT, glyphs);
    fonts.generation = generation;
}

/* ---- the slideshow ---- */

static int load_cover(fb_data *pixels, int album)
{
    struct tagcache_album al;
    char path[MAX_PATH];
    bool fallback;
    int size = art_cache_size_index("wps");
    if (size < 0 || !tagcache_album_get(album, &al) || !al.art_hash)
    {
        probe.no_key++;
        return -1;
    }
    if (!art_cache_lookup_hash(al.art_hash, size, path, sizeof(path), &fallback))
    {
        probe.no_file++;
        return -1;
    }
    if (fallback)
    {
        probe.fallback++;
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        probe.open_failures++;
        return -1;
    }
    struct bitmap bitmap = {
        .width = SHOW_EDGE, .height = SHOW_EDGE,
        .format = FORMAT_NATIVE, .data = (unsigned char *)pixels,
    };
    int result = art_cache_load_aat(fd, &bitmap, SHOW_BYTES, NULL);
    close(fd);
    if (result > 0)
        probe.loads++;
    else
        probe.decode_failures++;
    return result;
}

static void show_work(const struct queue_event *ev);
static struct bg_task show_task = { .name = "menu pane",
                                    .handle_event = show_work };

static void show_work(const struct queue_event *ev)
{
    (void)ev;
    if (show.state != SHOW_REQUESTED)
        return;
    if (show.release || show.handle <= 0)
    {
        show.state = SHOW_EMPTY;
        show.release = false;
        show.album = -1;
        if (show.handle > 0)
            show.handle = core_free(show.handle);
        return;
    }
    show.state = SHOW_LOADING;
    probe.work++;
    int count = tagcache_album_count();
    bool loaded = false;
    fb_data *pixels = core_get_data_pinned(show.handle);
    for (int i = 0; i < SHOW_TRIES && count > 0 && !show.release; i++)
    {
        /* USB and other work can keep this stopped across queue passes.
         * Let the pane retry on its timer so this worker can block. */
        if (bg_task_should_stop(&show_task))
        {
            probe.yielded++;
            break;
        }
        int album = rand() % count;
        if (count > 1 && album == show.album)
            album = (album + 1) % count;
        if (load_cover(pixels, album) > 0)
        {
            show.album = album;
            loaded = true;
            break;
        }
    }
    core_put_data_pinned(pixels);
    if (show.release)
    {
        show.release = false;
        show.handle = core_free(show.handle);
        show.state = SHOW_EMPTY;
        show.album = -1;
        return;
    }
    if (!loaded)
        show.none_at = USEC_TIMER;
    show.state = loaded ? SHOW_FRESH : SHOW_NONE;
}

static void show_request(void)
{
    probe.requests++;
    if (show.handle <= 0)
    {
        show.handle = core_alloc_free_space(SHOW_BYTES);
        if (show.handle <= 0)
        {
            probe.alloc_failures++;
            probe.available = core_available();
            probe.allocatable = core_allocatable();
            return;
        }
        show.album = -1;
    }
    show.state = SHOW_REQUESTED;
    bg_task_post(&show_task, SHOW_EVENT);
}

/* Whether the buffer holds a picture to draw. While the next cover loads
 * over it, or after a load that found none, the pane keeps the old one on
 * screen. */
static bool show_drawable(void)
{
    return show.state == SHOW_READY || show.state == SHOW_FRESH ||
           ((show.state == SHOW_REQUESTED || show.state == SHOW_LOADING ||
             show.state == SHOW_NONE) && show.album >= 0);
}

/* A new cover pans diagonally or straight up or down, from the end of its
 * range that leaves it the most travel. */
static void show_begin(uint32_t now)
{
    static const int8_t moves[][2] = {
        { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }, { 0, 1 }, { 0, -1 },
    };
    const int8_t *move = moves[rand() % ARRAYLEN(moves)];
    show.dx = move[0];
    show.dy = move[1];
    show.x0 = show.dx < 0 ? PAN_X_RANGE : show.dx > 0 ? 0 : PAN_X_RANGE / 2;
    show.y0 = show.dy < 0 ? PAN_Y_RANGE : show.dy > 0 ? 0 : PAN_Y_RANGE / 2;
    show.started = now;
    show.state = SHOW_READY;
}

/* The window over the cover at `now`. */
static void show_window(uint32_t now, int *x, int *y)
{
    if (show.held || show.state != SHOW_READY)
    {
        *x = show.drawn_x;
        *y = show.drawn_y;
        return;
    }
    int ticks = (int)MIN((now - show.started) / PAN_US_PER_PX, 1000u);
    *x = MAX(0, MIN(PAN_X_RANGE, show.x0 + show.dx * ticks));
    *y = MAX(0, MIN(PAN_Y_RANGE, show.y0 + show.dy * ticks));
}

/* Keep the last painted pose while Cover Flow owns the screen. The cache
 * worker only replaces the texture after the UI asks for a new cover. */
void menu_pane_hold_cover(void)
{
    if (shown.kind != LIST_PANE_ART || slide_dir)
        return;
    show.held = true;
    show.held_ready = show.state == SHOW_READY;
    show.keep_pixels = !show.held_ready || show.fading;
    show.held_at = show.keep_pixels ? USEC_TIMER : show.drawn_at;
}

const fb_data *menu_pane_pin_cover(struct ui_rect *area, int *stride)
{
    if (!show.held || show.keep_pixels || show.state != SHOW_READY ||
        show.handle <= 0)
        return NULL;
    *area = (struct ui_rect){ MENU_PANE_X - show.drawn_x, -show.drawn_y,
                              SHOW_EDGE, SHOW_EDGE };
    *stride = SHOW_EDGE;
    return core_get_data_pinned(show.handle);
}

void menu_pane_resume_cover(bool restored)
{
    if (!show.held)
        return;
    uint32_t now = USEC_TIMER;
    show.held = false;
    if (show.held_ready && show.state == SHOW_READY)
    {
        uint32_t pause = now - show.held_at;
        show.started += pause;
        show.fade_started += pause;
        show.drawn_at += pause;
    }
    if (restored && has_pending)
        pending_at = now;
    show.keep_pixels &= restored;
}

/* The cover's left edge on screen, with the window's column `x` at the
 * pane's left edge at rest. */
static int cover_left(uint32_t now, int x)
{
    int rest = MENU_PANE_X - x;
    if (!slide_dir)
        return rest;
    uint32_t t = MIN(now - slide_started, SLIDE_US);
    uint32_t q = slide_dir > 0 ? t : SLIDE_US - t;
    int travel = LCD_WIDTH - rest;
    return rest + (int)((uint64_t)travel * q * q /
                        ((uint64_t)SLIDE_US * SLIDE_US));
}

/* ---- painters ---- */

static fb_data *fb_at(int x, int y)
{
    return lcd_framebuffer_default.fb_ptr + y * LCD_WIDTH + x;
}

static struct ui_rect clip_to(struct ui_rect a, struct ui_rect b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.width, b.x + b.width);
    int y1 = MIN(a.y + a.height, b.y + b.height);
    return (struct ui_rect){ x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0) };
}

void menu_pane_backdrop(fb_data *buffer)
{
    for (int y = 0; y < LCD_HEIGHT; y++, buffer += LCD_WIDTH)
        stock_art_row(STOCK_ART_BACKGROUND_LIGHTBLUE, 0, y, buffer,
                      LCD_WIDTH);
}

void menu_pane_shadow(fb_data *row, int y, int edge, int limit)
{
    int from = MAX(edge, 0), to = MIN(edge + SHADOW_W, limit);
    if (to > from)
        stock_art_row(STOCK_ART_PREVIEWAREA_SHADOW, from - edge, y,
                      row + from, to - from);
}

/* The rows' shadow on the framebuffer inside `clip`. */
static void shade(struct ui_rect clip)
{
    struct ui_rect band = clip_to(clip, (struct ui_rect){
        MENU_PANE_X, 0, SHADOW_W, LCD_HEIGHT });
    for (int y = band.y; y < band.y + band.height; y++)
        stock_art_row(STOCK_ART_PREVIEWAREA_SHADOW, band.x - MENU_PANE_X, y,
                      fb_at(band.x, y), band.width);
}

/* The cover through the window at (`x`, `y`), its left edge at screen
 * column `left`. A cover away from rest shows a white leading edge. */
static void paint_cover(struct ui_rect clip, int x, int y, int left)
{
    const fb_data *pixels = core_get_data_pinned(show.handle);
    int from = MAX(clip.x, left);
    int to = MIN(clip.x + clip.width, left + SHOW_EDGE);
    for (int row = clip.y; row < clip.y + clip.height && to > from; row++)
    {
        const fb_data *src = pixels + (row + y) * SHOW_EDGE;
        memcpy(fb_at(from, row), src + (from - left),
               (to - from) * sizeof(fb_data));
    }
    core_put_data_pinned((void *)pixels);
    if (left > MENU_PANE_X - x)
    {
        struct ui_rect edge = clip_to(clip, (struct ui_rect){ left, 0, 2,
                                                              LCD_HEIGHT });
        for (int row = edge.y; row < edge.y + edge.height; row++)
            for (int col = edge.x; col < edge.x + edge.width; col++)
                *fb_at(col, row) = FB_RGBPACK(255, 255, 255);
    }
}

/* With five fractional bits, the RGB565 lanes share one multiply. */
static inline fb_data blend_pixel(fb_data to, fb_data from, unsigned weight)
{
    const uint32_t mask = 0x07e0f81f;
    uint32_t f = (to | ((uint32_t)to << 16)) & mask;
    uint32_t b = (from | ((uint32_t)from << 16)) & mask;
    uint32_t c = (b + (((f - b) * weight) >> 5)) & mask;
    return c | (c >> 16);
}

/* Move the pane on screen `weight` 32nds of the way to the cover at rest,
 * so that successive steps fade the old picture out. */
static void blend_cover(struct ui_rect clip, int x, int y, unsigned weight)
{
    const fb_data *pixels = core_get_data_pinned(show.handle);
    int column = clip.x - MENU_PANE_X;
    for (int row = clip.y; row < clip.y + clip.height; row++)
    {
        fb_data to[PANE_W];
        memcpy(to, pixels + (row + y) * SHOW_EDGE + x + column,
               clip.width * sizeof(fb_data));
        if (column < SHADOW_W)
            stock_art_row(STOCK_ART_PREVIEWAREA_SHADOW, column, row, to,
                          MIN(SHADOW_W - column, clip.width));
        fb_data *out = fb_at(clip.x, row);
        for (int i = 0; i < clip.width; i++)
            out[i] = blend_pixel(to[i], out[i], weight);
    }
    core_put_data_pinned((void *)pixels);
}

/* A still pane's text, laid out before its picture is composed. */
struct pane_lines {
    int count;
    struct {
        char text[TEXT_FIT_BUF];
        int font, x, top;
    } line[3];
};

/* Add `text`, centred on the pane with its baseline on `baseline`. */
static void pane_line(struct pane_lines *lines, const char *text, int font,
                      int baseline)
{
    if (font < 0 || !*text || lines->count >= (int)ARRAYLEN(lines->line))
        return;
    const int margin = 8;
    char *line = lines->line[lines->count].text;
    const char *fitted = text_fit(text, line, TEXT_FIT_BUF, font,
                                  PANE_W - 2 * margin);
    if (fitted != line)
        strlcpy(line, fitted, TEXT_FIT_BUF);
    int width;
    font_getstringsize((const unsigned char *)line, &width, NULL, font);
    lines->line[lines->count].font = font;
    lines->line[lines->count].x = PANE_MID - width / 2;
    lines->line[lines->count].top = baseline - font_get(font)->ascent;
    lines->count++;
}

/* A description in at most two lines, the last on `baseline`. */
static void put_wrapped(struct pane_lines *lines, const char *text, int font,
                        int baseline)
{
    if (font < 0)
        return;
    char first[sizeof(shown.caption)];
    strlcpy(first, text, sizeof(first));
    char *cut = NULL;
    for (char *space = strchr(first, ' '); space; space = strchr(space + 1, ' '))
    {
        int width;
        *space = '\0';
        font_getstringsize((const unsigned char *)first, &width, NULL, font);
        *space = ' ';
        if (width > WRAP_W)
            break;
        cut = space;
    }
    int whole;
    font_getstringsize((const unsigned char *)text, &whole, NULL, font);
    if (whole <= WRAP_W || !cut)
    {
        pane_line(lines, text, font, baseline);
        return;
    }
    *cut = '\0';
    pane_line(lines, first, font, baseline - 15);
    pane_line(lines, cut + 1, font, baseline);
}

/* A 119-pixel preview sits where stock puts them all, anything else centred
 * on the same point. A mask is white. */
static void put_picture(struct stock_canvas *band, int art)
{
    const struct stock_art *a = &stock_art[art];
    int x = PREVIEW_X, y = PREVIEW_Y;
    if (a->width != PREVIEW_EDGE || a->height != PREVIEW_EDGE)
    {
        x = PREVIEW_X + (PREVIEW_EDGE - a->width) / 2;
        y = PREVIEW_Y + (PREVIEW_EDGE - a->height) / 2;
    }
    if (a->colors)
        stock_art_put(band, art, x, y);
    else
        stock_art_mask(band, art, x, y, WHITE);
}

/* `id`, one column wide, drawn across columns [`from`, `to`). */
static void put_stretched(struct stock_canvas *band, enum stock_art_id id,
                          int from, int to)
{
    for (int x = from; x < to; x++)
        stock_art_put(band, id, x, CAPACITY_Y);
}

/* The capacity bar, `used` of its CAPACITY_W columns light from the left. */
static void put_capacity(struct stock_canvas *band, int used)
{
    const int x0 = CAPACITY_X, x1 = CAPACITY_X + CAPACITY_W;
    const int fill_cap =
        stock_art[STOCK_ART_CAPACITYVIEW_SIMPLE_FILLLEFT].width;
    const int empty_cap =
        stock_art[STOCK_ART_CAPACITYVIEW_SIMPLE_EMPTYLEFT].width;
    if (used >= CAPACITY_W - fill_cap)
    {
        stock_art_put(band, STOCK_ART_CAPACITYVIEW_SIMPLE_FILLLEFT, x0,
                      CAPACITY_Y);
        put_stretched(band, STOCK_ART_CAPACITYVIEW_SIMPLE_FILLCENTER,
                      x0 + fill_cap, x1 - fill_cap);
        stock_art_put(band, STOCK_ART_CAPACITYVIEW_SIMPLE_FILLRIGHT,
                      x1 - fill_cap, CAPACITY_Y);
        return;
    }
    int empty_from = x0 + empty_cap;
    if (used > 0)
    {
        used = MAX(used, fill_cap);
        stock_art_put(band, STOCK_ART_CAPACITYVIEW_SIMPLE_FILLLEFT, x0,
                      CAPACITY_Y);
        put_stretched(band, STOCK_ART_CAPACITYVIEW_SIMPLE_FILLCENTER,
                      x0 + fill_cap, x0 + used);
        empty_from = x0 + used;
    }
    else
        stock_art_put(band, STOCK_ART_CAPACITYVIEW_SIMPLE_EMPTYLEFT, x0,
                      CAPACITY_Y);
    put_stretched(band, STOCK_ART_CAPACITYVIEW_SIMPLE_EMPTYCENTER, empty_from,
                  x1 - empty_cap);
    stock_art_put(band, STOCK_ART_CAPACITYVIEW_SIMPLE_EMPTYRIGHT,
                  x1 - empty_cap, CAPACITY_Y);
}

/* The columns of the capacity bar that stand for used space. */
static int capacity_used(void)
{
    sector_t total = 0, free = 0;
    volume_size(IF_MV(0,) &total, &free);
    return total ? (int)((uint64_t)(total - free) * CAPACITY_W / total) : 0;
}

/* A still pane inside `band`: its picture, then its text. */
static void compose_still(struct stock_canvas *band,
                          const struct menu_pane_content *c, int used,
                          const struct pane_lines *lines)
{
    stock_art_put(band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    switch (c->kind)
    {
        case LIST_PANE_INFO:
            stock_art_put(band, STOCK_ART_SETTINGS_BASEPANEL, TILE_X, TILE_Y);
            if (c->art >= 0)
                stock_art_mask(band, c->art, TILE_X, TILE_Y, TILE_GLYPH);
            break;
        case LIST_PANE_DEVICE:
            stock_art_mask(band, STOCK_ART_ASTRAPOD_LOGO, LOGO_X, LOGO_Y,
                           WHITE);
            put_capacity(band, used);
            break;
        case LIST_PANE_SHUFFLE:
            stock_art_mask(band, STOCK_ART_MAINMENU_SHUFFLE, SHUFFLE_X,
                           SHUFFLE_Y, WHITE);
            break;
        case LIST_PANE_PICTURE:
            if (c->paint)
                c->paint(band);
            if (c->art >= 0)
                stock_art_put(band, c->art, c->x, c->y);
            break;
        default:
            if (c->art >= 0)
                put_picture(band, c->art);
            break;
    }
    for (int i = 0; i < lines->count; i++)
        stock_canvas_text(band, lines->line[i].font, lines->line[i].text,
                          lines->line[i].x, lines->line[i].top, WHITE);
    stock_art_put(band, STOCK_ART_PREVIEWAREA_SHADOW, MENU_PANE_X, 0);
}

static void playing_lines(struct pane_lines *lines)
{
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return;
    pane_line(lines, id3->title ? id3->title : "", fonts.title, 184);
    pane_line(lines, id3->artist ? id3->artist : "", fonts.text, 203);
    pane_line(lines, id3->album ? id3->album : "", fonts.text, 220);
}

static void free_line(struct pane_lines *lines)
{
    sector_t total = 0, free = 0;
    volume_size(IF_MV(0,) &total, &free);
    unsigned long long kib = (unsigned long long)free;
    bool gib = kib >= 1024ull * 1024;
    unsigned long long unit = gib ? 1024ull * 1024 : 1024;
    /* "Free:" without its colon, after the amount. */
    char label[32];
    strlcpy(label, str(LANG_DISK_FREE_INFO), sizeof(label));
    size_t n = strlen(label);
    if (n && label[n - 1] == ':')
        label[n - 1] = '\0';
    char text[64];
    snprintf(text, sizeof(text), "%u.%u %s %s", (unsigned)(kib / unit),
             (unsigned)(kib * 10 / unit % 10), gib ? "GB" : "MB", label);
    pane_line(lines, text, fonts.title, 213);
}

static void songs_line(struct pane_lines *lines)
{
    struct tagcache_marks marks;
    tagcache_get_marks(&marks);
    char text[48];
    snprintf(text, sizeof(text), "%d %s",
             MAX(0, tagcache_get_stat()->total_entries - (int)marks.deleted_ct),
             str(LANG_PV_K_SONGS));
    pane_line(lines, text, fonts.title, 206);
}

/* The text of the pane `c`. */
static void still_lines(struct pane_lines *lines,
                        const struct menu_pane_content *c)
{
    switch (c->kind)
    {
        case LIST_PANE_INFO:
            pane_line(lines, c->title, fonts.title, 34);
            put_wrapped(lines, c->caption, fonts.text, 216);
            break;
        case LIST_PANE_PLAYING:
            playing_lines(lines);
            break;
        case LIST_PANE_DEVICE:
            pane_line(lines, c->title, fonts.title, 34);
            free_line(lines);
            break;
        case LIST_PANE_SHUFFLE:
            pane_line(lines, c->title, fonts.title, 34);
            songs_line(lines);
            break;
        case LIST_PANE_PICTURE:
            pane_line(lines, c->title, fonts.title, c->baseline);
            pane_line(lines, c->caption, fonts.title, c->baseline + 21);
            break;
        default:
            pane_line(lines, c->title, fonts.title, 179);
            break;
    }
}

/* A pane on the backdrop, composed at 8 bits a channel as stock composes
 * it, text and all, and truncated once. */
static void paint_still(struct ui_rect clip, const struct menu_pane_content *c)
{
    struct pane_lines lines;
    lines.count = 0;
    still_lines(&lines, c);
    int used = c->kind == LIST_PANE_DEVICE ? capacity_used() : 0;
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            band_pixels, clip.x, y, clip.width,
            MIN(BAND_ROWS, clip.y + clip.height - y),
        };
        compose_still(&band, c, used, &lines);
        stock_canvas_show(&band, clip);
    }
}

static void paint_content(struct ui_rect clip, uint32_t now)
{
    clip = clip_to(clip, menu_pane_rect());
    if (clip.width <= 0 || clip.height <= 0)
        return;
    bool art = slide_dir || shown.kind == LIST_PANE_ART;
    if (art && show.state == SHOW_FRESH)
        show_begin(now);
    if (!art || !show_drawable())
    {
        if (art)
            probe.fallback_paints++;
        paint_still(clip, slide_dir ? &under : &shown);
        return;
    }
    int x, y;
    show_window(now, &x, &y);
    int left = cover_left(now, x);
    if (left > MENU_PANE_X)
        paint_still(clip_to(clip, (struct ui_rect){ MENU_PANE_X, 0,
                                                    left - MENU_PANE_X,
                                                    LCD_HEIGHT }), &under);
    paint_cover(clip, x, y, left);
    probe.cover_paints++;
    shade(clip_to(clip, (struct ui_rect){ left, 0, LCD_WIDTH - left,
                                          LCD_HEIGHT }));
    show.drawn_x = x;
    show.drawn_y = y;
    if (!show.held)
        show.drawn_at = now;
}

void menu_pane_paint(struct ui_rect clip)
{
    /* The retained pane is already in the framebuffer. Resume an in-flight
     * blend on its pixels, including when the list repaints on return. */
    if (shown.kind == LIST_PANE_ART && show.keep_pixels)
        return;
    load_fonts();
    paint_content(clip, USEC_TIMER);
    ultra_pane_repaint(clip);
}

/* ---- the content shown ---- */

static bool same_content(const struct menu_pane_content *a,
                         const struct menu_pane_content *b)
{
    return a->kind == b->kind && a->art == b->art &&
           !strcmp(a->title, b->title) && !strcmp(a->caption, b->caption) &&
           a->x == b->x && a->y == b->y && a->baseline == b->baseline &&
           a->paint == b->paint && a->stamp == b->stamp;
}

void menu_pane_select(const struct menu_pane_content *content,
                      unsigned list_identity)
{
    if (content->kind == LIST_PANE_PICTURE)
        content = &picture;
    bool new_list = list_identity != identity;
    identity = list_identity;
    if (new_list)
    {
        ultra_pane_cancel();
        if (content->kind != shown.kind)
            show.keep_pixels = false;
        shown = *content;
        has_pending = false;
        slide_dir = 0;
    }
    else if (!same_content(content, has_pending ? &pending : &shown))
    {
        if (same_content(content, &shown))
            has_pending = false;
        else
        {
            pending = *content;
            has_pending = true;
            pending_at = USEC_TIMER;
        }
    }
    if (shown.kind == LIST_PANE_ART && show.state == SHOW_EMPTY)
        show_request();
}

void menu_pane_set_picture(const struct menu_pane_content *content)
{
    picture = *content;
    picture.kind = LIST_PANE_PICTURE;
}

/* ---- motion ---- */

static void cover_step(uint32_t now, struct ui_rect *damage)
{
    struct ui_rect pane = menu_pane_rect();
    /* Retry an interrupted or unsuccessful load a cover's life later. */
    if (show.state == SHOW_NONE && now - show.none_at >= SHOW_LIFE_US)
    {
        show_request();
        return;
    }
    if (show.state == SHOW_FRESH)
    {
        show_begin(now);
        show.fading = show.drawn_x >= 0;
        show.fade_started = now;
        show.fade_level = 0;
    }
    if (show.state != SHOW_READY)
        return;
    int x, y;
    show_window(now, &x, &y);
    if (show.fading)
    {
        uint32_t t = now - show.fade_started;
        int level = t >= FADE_US ? 32 : (int)(t * 32 / FADE_US);
        if (level != show.fade_level)
        {
            /* Each step removes its share of what is left of the old. */
            unsigned weight = level >= 32 ? 32 :
                (level - show.fade_level) * 32 / (32 - show.fade_level);
            blend_cover(pane, x, y, MAX(weight, 1u));
            show.fade_level = level;
            show.drawn_x = x;
            show.drawn_y = y;
            show.drawn_at = now;
            *damage = pane;
        }
        if (level >= 32)
            show.fading = show.keep_pixels = false;
        return;
    }
    if (x != show.drawn_x || y != show.drawn_y)
    {
        paint_cover(pane, x, y, MENU_PANE_X - x);
        shade(pane);
        show.drawn_x = x;
        show.drawn_y = y;
        show.drawn_at = now;
        *damage = pane;
    }
    uint32_t age = now - show.started;
    if (age >= SHOW_LIFE_US &&
        (storage_disk_is_active() || age >= IDLE_DWELL_US))
        show_request();
}

static bool playing_changed(void)
{
    if (shown.kind != LIST_PANE_PLAYING || TIME_BEFORE(current_tick,
                                                       playing_polled))
        return false;
    playing_polled = current_tick + PLAYING_POLL;
    struct mp3entry *id3 = audio_current_track();
    char key[sizeof(playing_key)];
    snprintf(key, sizeof(key), "%s|%s|%s", id3 && id3->title ? id3->title : "",
             id3 && id3->artist ? id3->artist : "",
             id3 && id3->album ? id3->album : "");
    if (!strcmp(key, playing_key))
        return false;
    strcpy(playing_key, key);
    return true;
}

struct ui_rect menu_pane_step(void)
{
    struct ui_rect damage = { 0 };
    if (!menu_pane_split())
    {
        ultra_pane_cancel();
        return damage;
    }
    if (show.held || lcd_updates_held())
    {
        probe.held_steps++;
        return damage;
    }
    probe.steps++;
    load_fonts();
    uint32_t now = USEC_TIMER;
    bool repaint = false, dissolve = false;
    if (has_pending && now - pending_at >= CHANGE_DELAY_US)
    {
        bool from_art = shown.kind == LIST_PANE_ART;
        bool to_art = pending.kind == LIST_PANE_ART;
        under = from_art ? pending : shown;
        shown = pending;
        if (from_art != to_art)
            show.keep_pixels = false;
        has_pending = false;
        if (to_art && show.state == SHOW_EMPTY)
            show_request();
        /* A cover leaves to the right, or arrives from there. */
        if (from_art != to_art && show.state == SHOW_READY)
        {
            slide_dir = from_art ? 1 : -1;
            slide_started = now;
        }
        dissolve = !slide_dir && ui_theme_ultra() &&
                   ultra_pane_hold(menu_pane_rect());
        repaint = true;
    }
    else if (ultra_pane_active())
        damage = ultra_pane_step();
    else if (slide_dir)
    {
        if (now - slide_started >= SLIDE_US)
            slide_dir = 0;
        repaint = true;
    }
    else if (shown.kind == LIST_PANE_ART)
        cover_step(now, &damage);
    else
        repaint = playing_changed();
    if (repaint)
    {
        damage = menu_pane_rect();
        paint_content(damage, now);
        if (dissolve)
            damage = ultra_pane_begin();
    }
    stepped_at = now;
    return damage;
}

#if defined(HAVE_USBSTACK)
void menu_pane_status(char *out, size_t size)
{
    struct tagcache_marks marks;
    tagcache_get_marks(&marks);
    const struct bg_marks *done = &art_cache_task.done_marks;
    struct art_cache_counts counts;
    art_cache_get_counts(&counts);
    snprintf(out, size,
             "split=%d row=%d art=%d state=%d handle=%d album=%d "
             "db_ram=%d db_albums=%d db_marks=%d/%d/%d "
             "cache_busy=%d cache_marks=%d/%d/%d pass_covers=%d/%d "
             "requests=%u alloc_failures=%u alloc_free=%lu alloc_max=%lu "
             "work=%u yielded=%u no_key=%u no_file=%u fallback=%u "
             "open_failures=%u decode_failures=%u loads=%u "
             "cover_paints=%u fallback_paints=%u steps=%u held_steps=%u",
             menu_pane_split(), shown.kind, shown.kind == LIST_PANE_ART,
             show.state, show.handle, show.album, tagcache_is_in_ram(),
             tagcache_album_count(), tagcache_get_stat()->total_entries,
             (int)marks.commitid, (int)marks.deleted_ct, art_cache_is_busy(),
             done->entries, done->commitid,
             done->deleted, counts.album_art, counts.albums, probe.requests,
             probe.alloc_failures, (unsigned long)probe.available,
             (unsigned long)probe.allocatable, probe.work, probe.yielded,
             probe.no_key, probe.no_file, probe.fallback, probe.open_failures,
             probe.decode_failures, probe.loads, probe.cover_paints,
             probe.fallback_paints, probe.steps, probe.held_steps);
}
#endif

int menu_pane_deadline(void)
{
    if (!menu_pane_split() || show.held)
        return TIMEOUT_BLOCK;
    if (ultra_pane_active())
        return ultra_pane_deadline();
    uint32_t now = USEC_TIMER;
    uint32_t wait = UINT32_MAX;
    if (has_pending)
    {
        uint32_t age = now - pending_at;
        wait = age >= CHANGE_DELAY_US ? 0 : CHANGE_DELAY_US - age;
    }
    uint32_t since = now - stepped_at;
    uint32_t frame = since >= FRAME_US ? 0 : FRAME_US - since;
    if (slide_dir)
        wait = MIN(wait, frame);
    else if (shown.kind == LIST_PANE_ART)
    {
        if (show.state == SHOW_FRESH)
            wait = 0;
        else if (show.fading)
            wait = MIN(wait, frame);
        else if (show.state == SHOW_NONE)
        {
            uint32_t age = now - show.none_at;
            wait = MIN(wait, age >= SHOW_LIFE_US ? 0 : SHOW_LIFE_US - age);
        }
        else if (show.state == SHOW_READY)
        {
            uint32_t age = now - show.started;
            int x, y;
            show_window(now, &x, &y);
            uint32_t next = (age / PAN_US_PER_PX + 1) * PAN_US_PER_PX - age;
            if (x != show.drawn_x || y != show.drawn_y ||
                (age >= SHOW_LIFE_US &&
                 (storage_disk_is_active() || age >= IDLE_DWELL_US)))
                next = 0;
            else if (age < SHOW_LIFE_US)
                next = MIN(next, SHOW_LIFE_US - age);
            wait = MIN(wait, next);
        }
        else
            wait = MIN(wait, 100000u);
    }
    else if (shown.kind == LIST_PANE_PLAYING)
        wait = MIN(wait, (uint32_t)PLAYING_POLL * (1000000 / HZ));
    if (wait == UINT32_MAX)
        return TIMEOUT_BLOCK;
    return (wait + 1000000 / HZ - 1) / (1000000 / HZ);
}
