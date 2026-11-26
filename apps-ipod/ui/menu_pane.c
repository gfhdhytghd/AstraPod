/***************************************************************************
 * PodBox split-screen menu pane. GPL version 2+.
 *
 * The right half of a menu under style.menu_pane=split, as the stock iPod
 * Classic draws it: a slideshow of the library's covers, or on a grey
 * backdrop the playing track, the player's free space, or the selected row's
 * name, icon and value. The rows cast a shadow on its first columns.
 *
 * Parts: the page on screen and its depth; the fonts; the slideshow, whose
 * covers the background thread reads into one buffer; the painters; the
 * content shown and the pause before a new row's; and the motion.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "file.h"
#include "font.h"
#include "rbunicode.h"
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
#include "animation.h"
#include "menu_pane.h"
#include "retained.h"
#include "native_theme_runtime.h"

#define PANE_W (LCD_WIDTH - MENU_PANE_X)
#define PANE_MID (MENU_PANE_X + PANE_W / 2)
#define SHADOW_W 25
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
#define TILE 78
#define TILE_REFLECTION 35
#define WRAP_W 132

enum show_state { SHOW_EMPTY, SHOW_REQUESTED, SHOW_LOADING, SHOW_FRESH,
                  SHOW_READY, SHOW_NONE };

static struct {
    int depth;
    bool split, on_screen;
} page = { -1, false, false };

static struct {
    unsigned generation;
    int title, text, icon;
} fonts = { (unsigned)-1, -1, -1, -1 };

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
    bool fading;
} show = { .album = -1 };

static struct menu_pane_content shown, pending, under;
static unsigned identity;
static bool has_pending;
static uint32_t pending_at, slide_started, stepped_at;
/* 1 while the cover leaves to the right, -1 while it arrives from there,
 * with `under` the pane beneath it. */
static int slide_dir;
static char playing_key[300];
static long playing_polled;

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
}

void menu_pane_page(bool split)
{
    page.split = page.on_screen = split;
    page.depth = viewportmanager_depth(SCREEN_MAIN);
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
    font_unload(fonts.icon);
    int glyphs = global_settings.glyphs_to_cache;
    fonts.title = ui_theme_current_font_load(UI_FONT_PANE_TITLE, glyphs);
    fonts.text = ui_theme_current_font_load(UI_FONT_PANE_TEXT, glyphs);
    fonts.icon = ui_theme_current_font_load(UI_FONT_ICON, glyphs);
    fonts.generation = generation;
}

/* ---- the slideshow ---- */

static int load_cover(fb_data *pixels, int album)
{
    struct tagcache_album al;
    char path[MAX_PATH];
    bool fallback;
    int size = art_cache_size_index("wps");
    if (size < 0 || !tagcache_album_get(album, &al) || !al.art_hash ||
        !art_cache_lookup_hash(al.art_hash, size, path, sizeof(path),
                               &fallback) || fallback)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct bitmap bitmap = {
        .width = SHOW_EDGE, .height = SHOW_EDGE,
        .format = FORMAT_NATIVE, .data = (unsigned char *)pixels,
    };
    int result = art_cache_load_aat(fd, &bitmap, SHOW_BYTES, NULL);
    close(fd);
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
    int count = tagcache_album_count();
    bool loaded = false, yielded = false;
    fb_data *pixels = core_get_data_pinned(show.handle);
    for (int i = 0; i < SHOW_TRIES && count > 0 && !show.release; i++)
    {
        /* Glyphs for the page being drawn come first. */
        if (bg_task_should_stop(&show_task))
        {
            yielded = true;
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
    if (yielded && !show.release)
    {
        show.state = SHOW_REQUESTED;
        bg_task_post(&show_task, SHOW_EVENT);
        return;
    }
    if (show.release)
    {
        show.release = false;
        show.handle = core_free(show.handle);
        show.state = SHOW_EMPTY;
        show.album = -1;
        return;
    }
    show.state = loaded ? SHOW_FRESH : SHOW_NONE;
}

static void show_request(void)
{
    if (show.handle <= 0)
    {
        show.handle = core_alloc_free_space(SHOW_BYTES);
        if (show.handle <= 0)
            return;
        show.album = -1;
    }
    show.state = SHOW_REQUESTED;
    bg_task_post(&show_task, SHOW_EVENT);
}

/* Whether the buffer holds a picture to draw. While the next cover loads
 * over it the pane keeps the old one on screen. */
static bool show_drawable(void)
{
    return show.state == SHOW_READY || show.state == SHOW_FRESH ||
           ((show.state == SHOW_REQUESTED || show.state == SHOW_LOADING) &&
            show.album >= 0);
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
    if (show.state != SHOW_READY)
    {
        *x = show.drawn_x;
        *y = show.drawn_y;
        return;
    }
    int ticks = (int)MIN((now - show.started) / PAN_US_PER_PX, 1000u);
    *x = MAX(0, MIN(PAN_X_RANGE, show.x0 + show.dx * ticks));
    *y = MAX(0, MIN(PAN_Y_RANGE, show.y0 + show.dy * ticks));
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

/* The grey backdrop: brighter at the top and towards the rows, sampled
 * from the stock pane every 20 rows at its two edges. */
static const uint32_t backdrop_left[] = {
    0x98acba, 0x9bb0bd, 0x98aaba, 0x93a6b3, 0x8b9bac, 0x7f949f, 0x748391,
    0x677682, 0x5d6774, 0x535e67, 0x4c5660, 0x4b5359, 0x464f55,
};
static const uint32_t backdrop_right[] = {
    0x8597a4, 0x8496a5, 0x80929e, 0x7a8b98, 0x74838f, 0x6c7d87, 0x63717c,
    0x5c6871, 0x535f68, 0x4d5761, 0x495059, 0x454f56, 0x434c52,
};

static uint32_t mix_rgb(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8)
    {
        int ca = (a >> shift) & 255, cb = (b >> shift) & 255;
        out |= (uint32_t)(ca + (cb - ca) * num / den) << shift;
    }
    return out;
}

static void paint_backdrop(struct ui_rect clip)
{
    for (int y = clip.y; y < clip.y + clip.height; y++)
    {
        int band = MIN(y / 20, (int)ARRAYLEN(backdrop_left) - 2);
        int part = y - band * 20;
        uint32_t left = mix_rgb(backdrop_left[band], backdrop_left[band + 1],
                                part, 20);
        uint32_t right = mix_rgb(backdrop_right[band],
                                 backdrop_right[band + 1], part, 20);
        /* 16.16 channels stepped across the row. */
        int32_t c[3], step[3];
        for (int i = 0; i < 3; i++)
        {
            int l = (left >> (16 - 8 * i)) & 255;
            int r = (right >> (16 - 8 * i)) & 255;
            step[i] = ((r - l) << 16) / (PANE_W - 1);
            c[i] = (l << 16) + step[i] * (clip.x - MENU_PANE_X);
        }
        fb_data *out = fb_at(clip.x, y);
        for (int x = 0; x < clip.width; x++)
        {
            *out++ = FB_RGBPACK(c[0] >> 16, c[1] >> 16, c[2] >> 16);
            for (int i = 0; i < 3; i++)
                c[i] += step[i];
        }
    }
}

/* The shadow the rows cast on the pane, as the brightness left in each of
 * its first columns, measured from the stock pane. */
static const uint8_t shadow_keep[SHADOW_W] = {
    139, 150, 159, 168, 176, 182, 188, 195, 202, 209, 215, 221, 227,
    232, 237, 242, 245, 247, 249, 251, 252, 254, 254, 254, 254,
};

void menu_pane_shadow(fb_data *row, int edge, int limit)
{
    for (int x = MAX(edge, 0); x < MIN(edge + SHADOW_W, limit); x++)
        ui_blit_dim(row + x, row + x, 1, shadow_keep[x - edge]);
}

static void shade(struct ui_rect clip)
{
    struct ui_rect band = clip_to(clip, (struct ui_rect){
        MENU_PANE_X, 0, SHADOW_W, LCD_HEIGHT });
    for (int y = band.y; y < band.y + band.height; y++)
    {
        fb_data *out = fb_at(band.x, y);
        for (int x = band.x; x < band.x + band.width; x++, out++)
            ui_blit_dim(out, out, 1, shadow_keep[x - MENU_PANE_X]);
    }
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
    for (int row = clip.y; row < clip.y + clip.height; row++)
    {
        const fb_data *src = pixels + (row + y) * SHOW_EDGE + x +
                             (clip.x - MENU_PANE_X);
        fb_data *out = fb_at(clip.x, row);
        for (int i = 0; i < clip.width; i++)
        {
            fb_data to = src[i];
            int column = clip.x + i - MENU_PANE_X;
            if (column < SHADOW_W)
                ui_blit_dim(&to, &to, 1, shadow_keep[column]);
            out[i] = blend_pixel(to, out[i], weight);
        }
    }
    core_put_data_pinned((void *)pixels);
}

/* `text` centred on the pane with its baseline on `baseline`. */
static void pane_line(struct ui_rect clip, const char *text, int font,
                     int baseline)
{
    if (font < 0 || !*text)
        return;
    struct font *pf = font_get(font);
    int top = baseline - pf->ascent;
    struct ui_rect area = clip_to(clip, (struct ui_rect){
        MENU_PANE_X, top, PANE_W, pf->height });
    if (area.width <= 0 || area.height <= 0)
        return;
    char fitted[TEXT_FIT_BUF];
    const int margin = 8;
    text = text_fit(text, fitted, sizeof(fitted), font, PANE_W - 2 * margin);
    int width;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    struct viewport vp = {
        .x = area.x, .y = area.y, .width = area.width, .height = area.height,
        .font = font, .drawmode = DRMODE_FG,
        .fg_pattern = LCD_RGBPACK(255, 255, 255),
        .buffer = &lcd_framebuffer_default, .flags = VP_DEFAULT_FLAGS,
    };
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_putsxy(PANE_MID - width / 2 - area.x, top - area.y,
               (const unsigned char *)text);
    lcd_set_viewport(prior);
}

/* A description in at most two lines, the last on `baseline`. */
static void put_wrapped(struct ui_rect clip, const char *text, int font,
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
        pane_line(clip, text, font, baseline);
        return;
    }
    *cut = '\0';
    pane_line(clip, first, font, baseline - 15);
    pane_line(clip, cut + 1, font, baseline);
}

/* The row's icon glyph in white, `scale` times its size, centred on
 * (`cx`, `cy`). */
static void put_icon(struct ui_rect clip, const char *glyph, int cx, int cy,
                     int scale)
{
    enum { CELL = 32 };
    if (!*glyph || fonts.icon < 0)
        return;
    struct font *pf = font_get(fonts.icon);
    ucschar_t ch;
    utf8decode((const unsigned char *)glyph, &ch);
    const unsigned char *bits = font_get_bits(pf, ch);
    int width = font_get_width(pf, ch);
    int height = pf->height;
    if (!bits || !pf->depth || width <= 0 || width > CELL || height > CELL)
        return;
    int x0 = cx - width * scale / 2;
    int y0 = cy - height * scale / 2;
    struct ui_rect area = clip_to(clip, (struct ui_rect){
        x0, y0, width * scale, height * scale });
    const fb_data white = FB_RGBPACK(255, 255, 255);
    for (int y = area.y; y < area.y + area.height; y++)
    {
        int fy = ((y - y0) * 2 + 1) * 128 / scale - 128;
        for (int x = area.x; x < area.x + area.width; x++)
        {
            /* Bilinear over the 4bpp coverage, packed across rows with the
             * low nibble first and 0 opaque. */
            int fx = ((x - x0) * 2 + 1) * 128 / scale - 128;
            int ix = fx >> 8, iy = fy >> 8, wx = fx & 255, wy = fy & 255;
            unsigned cover = 0;
            for (int k = 0; k < 4; k++)
            {
                int px = ix + (k & 1), py = iy + (k >> 1);
                if (px < 0 || px >= width || py < 0 || py >= height)
                    continue;
                int at = py * width + px;
                unsigned byte = bits[at / 2];
                unsigned c = 15 - ((at & 1) ? byte >> 4 : byte & 15);
                cover += c * ((k & 1) ? wx : 256 - wx) *
                             ((k >> 1) ? wy : 256 - wy);
            }
            unsigned alpha = cover / (15 * 256);
            if (!alpha)
                continue;
            fb_data *out = fb_at(x, y);
            ui_blit_mix(out, &white, out, 1, alpha, UI_FULL_OPACITY - alpha);
        }
    }
}

/* A settings icon: a black glossy tile with rounded corners and its
 * reflection, the glyph in white on it. */
static void put_tile(struct ui_rect clip, const char *glyph)
{
    static const uint8_t corner[] = { 5, 2, 1 };
    const int x0 = PANE_MID - TILE / 2, y0 = 81;
    for (int r = 0; r < TILE + TILE_REFLECTION; r++)
    {
        int y = y0 + r;
        if (y < clip.y || y >= clip.y + clip.height)
            continue;
        int mirrored = r < TILE ? r : 2 * TILE - 1 - r;
        int edge = MIN(mirrored, TILE - 1 - mirrored);
        int inset = edge < (int)ARRAYLEN(corner) ? corner[edge] : 0;
        int from = MAX(clip.x, x0 + inset);
        int to = MIN(clip.x + clip.width, x0 + TILE - inset);
        if (to <= from)
            continue;
        fb_data *out = fb_at(from, y);
        if (r < TILE)
        {
            unsigned v = r == 0 ? 0x40 : r == 1 ? 0xa1 :
                         r < 39 ? 0x79 - (0x79 - 0x23) * (r - 2) / 36 : 0;
            fb_data fill = FB_RGBPACK(v, v, v);
            for (int x = from; x < to; x++)
                *out++ = fill;
        }
        else
        {
            /* The reflection darkens the backdrop by 53% and fades out. */
            int k = r - TILE;
            unsigned alpha = 136 - 136 * k / TILE_REFLECTION;
            ui_blit_dim(out, out, to - from, UI_FULL_OPACITY - alpha);
        }
    }
    put_icon(clip, glyph, PANE_MID, y0 + TILE / 2, 2);
}

static void put_capsule(struct ui_rect clip, int used_width)
{
    static const uint32_t used[] = {
        0x5e6164, 0xf6f9fa, 0xf1f2f6, 0xeaedf0, 0xe1e5e7, 0xdadde0, 0xcfd4d7,
        0xc7cbcf, 0xbec2c8, 0xb5bbc1, 0xafb5bb, 0xabb1b7, 0x727b84,
    };
    static const uint32_t unused[] = {
        0x343b43, 0x414448, 0x42464a, 0x45494c, 0x474b50, 0x4a4f55, 0x4d5259,
        0x50575d, 0x535a62, 0x565d65, 0x596069, 0x515a62, 0x727b84,
    };
    static const uint8_t inset[] = { 5, 3, 2, 1, 0, 0, 0, 0, 0, 1, 2, 3, 5 };
    const int x0 = 179, width = 129, y0 = 173;
    for (int r = 0; r < (int)ARRAYLEN(used); r++)
    {
        int y = y0 + r;
        if (y < clip.y || y >= clip.y + clip.height)
            continue;
        int from = MAX(clip.x, x0 + inset[r]);
        int to = MIN(clip.x + clip.width, x0 + width - inset[r]);
        for (int x = from; x < to; x++)
        {
            uint32_t c = x < x0 + used_width ? used[r] : unused[r];
            *fb_at(x, y) = FB_RGBPACK(c >> 16, (c >> 8) & 255, c & 255);
        }
    }
}

static void paint_category(struct ui_rect clip, const struct menu_pane_content *c)
{
    put_icon(clip, c->glyph, PANE_MID, 103, 3);
    pane_line(clip, c->title, fonts.title, 178);
}

static void paint_info(struct ui_rect clip, const struct menu_pane_content *c)
{
    pane_line(clip, c->title, fonts.title, 33);
    put_tile(clip, c->glyph);
    put_wrapped(clip, c->caption, fonts.text, 215);
}

static void paint_playing(struct ui_rect clip, const struct menu_pane_content *c)
{
    put_icon(clip, c->glyph, PANE_MID, 103, 3);
    struct mp3entry *id3 = audio_current_track();
    if (!id3)
        return;
    pane_line(clip, id3->title ? id3->title : "", fonts.title, 183);
    pane_line(clip, id3->artist ? id3->artist : "", fonts.text, 202);
    pane_line(clip, id3->album ? id3->album : "", fonts.text, 219);
}

static void paint_device(struct ui_rect clip, const struct menu_pane_content *c)
{
    pane_line(clip, c->title, fonts.title, 33);
    put_icon(clip, c->glyph, PANE_MID, 103, 3);
    sector_t total = 0, free = 0;
    volume_size(IF_MV(0,) &total, &free);
    put_capsule(clip, total ? (int)((uint64_t)(total - free) * 129 / total) : 0);
    char text[48];
    unsigned long long kib = (unsigned long long)free;
    bool gib = kib >= 1024ull * 1024;
    unsigned long long unit = gib ? 1024ull * 1024 : 1024;
    snprintf(text, sizeof(text), "%s %llu.%llu %s", str(LANG_DISK_FREE_INFO),
             kib / unit, kib * 10 / unit % 10, gib ? "GB" : "MB");
    pane_line(clip, text, fonts.title, 212);
}

/* A pane on the grey backdrop. */
static void paint_still(struct ui_rect clip, const struct menu_pane_content *c)
{
    paint_backdrop(clip);
    switch (c->kind)
    {
        case LIST_PANE_INFO: paint_info(clip, c); break;
        case LIST_PANE_PLAYING: paint_playing(clip, c); break;
        case LIST_PANE_DEVICE: paint_device(clip, c); break;
        default: paint_category(clip, c); break;
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
    show.drawn_x = x;
    show.drawn_y = y;
}

void menu_pane_paint(struct ui_rect clip)
{
    load_fonts();
    paint_content(clip, USEC_TIMER);
    shade(clip);
}

/* ---- the content shown ---- */

static bool same_content(const struct menu_pane_content *a,
                         const struct menu_pane_content *b)
{
    return a->kind == b->kind && !strcmp(a->title, b->title) &&
           !strcmp(a->caption, b->caption) && !strcmp(a->glyph, b->glyph);
}

void menu_pane_select(const struct menu_pane_content *content,
                      unsigned list_identity)
{
    bool new_list = list_identity != identity;
    identity = list_identity;
    if (new_list)
    {
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

/* ---- motion ---- */

static void cover_step(uint32_t now, struct ui_rect *damage)
{
    struct ui_rect pane = menu_pane_rect();
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
            *damage = pane;
        }
        if (level >= 32)
            show.fading = false;
        return;
    }
    if (x != show.drawn_x || y != show.drawn_y)
    {
        paint_cover(pane, x, y, MENU_PANE_X - x);
        shade(pane);
        show.drawn_x = x;
        show.drawn_y = y;
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
    if (!menu_pane_split() || lcd_updates_held())
        return damage;
    load_fonts();
    uint32_t now = USEC_TIMER;
    bool repaint = false;
    if (has_pending && now - pending_at >= CHANGE_DELAY_US)
    {
        bool from_art = shown.kind == LIST_PANE_ART;
        bool to_art = pending.kind == LIST_PANE_ART;
        under = from_art ? pending : shown;
        shown = pending;
        has_pending = false;
        if (to_art && show.state == SHOW_EMPTY)
            show_request();
        /* A cover leaves to the right, or arrives from there. */
        if (from_art != to_art && show.state == SHOW_READY)
        {
            slide_dir = from_art ? 1 : -1;
            slide_started = now;
        }
        repaint = true;
    }
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
        shade(damage);
    }
    stepped_at = now;
    return damage;
}

int menu_pane_deadline(void)
{
    if (!menu_pane_split())
        return TIMEOUT_BLOCK;
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
        else if (show.state == SHOW_READY)
        {
            uint32_t age = now - show.started;
            uint32_t next = (age / PAN_US_PER_PX + 1) * PAN_US_PER_PX - age;
            if (age < SHOW_LIFE_US)
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
