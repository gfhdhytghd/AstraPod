/***************************************************************************
 * PodBox: the stock iPod Classic firmware's bitmaps. GPL version 2+.
 *
 * The art's pixels come from STOCK_ART_PACK, which bundle-theme.sh writes
 * from the PNGs in apps-ipod/bitmaps/source/stock/: it is too large to build
 * into the firmware. Drawing goes onto an 8-bit canvas, which a caller
 * writes to the framebuffer once, or straight into the framebuffer.
 * Every blend is the one stock uses, (d * (256 - a') + s * a') >> 8 per
 * channel with a' = a + (a >> 7).
 ****************************************************************************/

#include <stddef.h>
#include "config.h"
#include "system.h"
#include "bidi.h"
#include "core_alloc.h"
#include "file.h"
#include "font.h"
#include "logf.h"
#include "stock_art.h"

#define PACK_MAGIC 0x54524153u /* "SART" */
#define PACK_VERSION 1

static int pack_handle;
static bool pack_failed;
static bool over_layer;

/* Buflib moves the pack only while the UI thread is away, so repointing the
 * table here keeps every pointer good. */
static int pack_move(int handle, void *current, void *new)
{
    (void)handle;
    intptr_t delta = (char *)new - (char *)current;
    for (int i = 0; i < STOCK_ART_COUNT; i++)
    {
        if (stock_art[i].palette)
            stock_art[i].palette = (const uint32_t *)
                ((const char *)stock_art[i].palette + delta);
        stock_art[i].pixels = (const char *)stock_art[i].pixels + delta;
    }
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks pack_ops = { .move_callback = pack_move };

/* Point the table into the pack at `base` of `size` bytes, or say why not. */
static bool pack_bind(const char *base, size_t size)
{
    const uint32_t *head = (const uint32_t *)base;
    if (size < 16 || head[0] != PACK_MAGIC || head[1] != PACK_VERSION ||
        head[2] != STOCK_ART_COUNT || head[3] != STOCK_ART_FINGERPRINT ||
        size < 16 + 8 * (size_t)STOCK_ART_COUNT)
        return false;
    const uint32_t *offsets = head + 4;
    for (int i = 0; i < STOCK_ART_COUNT; i++)
    {
        struct stock_art *art = &stock_art[i];
        size_t texel = !art->colors || art->colors <= 256 ? 1 : 2;
        size_t pal = offsets[2 * i], pix = offsets[2 * i + 1];
        if ((art->colors && (!pal || pal + 4u * art->colors > size)) ||
            !pix || pix + texel * art->width * art->height > size)
            return false;
        art->palette = art->colors ? (const uint32_t *)(base + pal) : NULL;
        art->pixels = base + pix;
    }
    return true;
}

bool stock_art_ready(void)
{
    if (pack_handle > 0)
        return true;
    if (pack_failed)
        return false;
    pack_failed = true;
    int fd = open(STOCK_ART_PACK, O_RDONLY);
    if (fd < 0)
    {
        logf("stock art: no %s", STOCK_ART_PACK);
        return false;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    int handle = size > 0 ? core_alloc_ex(size, &pack_ops) : 0;
    if (handle > 0)
    {
        char *base = core_get_data_pinned(handle);
        bool ok = read(fd, base, size) == size && pack_bind(base, size);
        core_put_data_pinned(base);
        if (ok)
        {
            pack_handle = handle;
            pack_failed = false;
        }
        else
        {
            logf("stock art: %s does not match this firmware",
                 STOCK_ART_PACK);
            core_free(handle);
        }
    }
    close(fd);
    return pack_handle > 0;
}

void stock_art_release(void)
{
    if (pack_handle > 0)
        pack_handle = core_free(pack_handle);
    for (int i = 0; i < STOCK_ART_COUNT; i++)
        stock_art[i].palette = stock_art[i].pixels = NULL;
    pack_failed = false;
}

static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a)
{
    a += a >> 7;
    uint32_t rb = ((d & 0xff00ff) * (256 - a) + (s & 0xff00ff) * a) >> 8;
    uint32_t g = ((d & 0x00ff00) * (256 - a) + (s & 0x00ff00) * a) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

/* The pixel's colour and alpha, as ARGB8888. */
static inline uint32_t texel(const struct stock_art *art, int i)
{
    if (!art->colors)
        return (uint32_t)((const uint8_t *)art->pixels)[i] << 24;
    unsigned n = art->colors <= 256 ? ((const uint8_t *)art->pixels)[i] :
                                      ((const uint16_t *)art->pixels)[i];
    return art->palette[n];
}

static inline uint32_t widen(fb_data p)
{
    return (uint32_t)FB_UNPACK_RED(p) << 16 |
           (uint32_t)FB_UNPACK_GREEN(p) << 8 | FB_UNPACK_BLUE(p);
}

static inline fb_data narrow(uint32_t c)
{
    return FB_RGBPACK(c >> 16, (c >> 8) & 255, c & 255);
}

/* The rectangle of `art` at (x, y) inside both `clip` and the screen. */
static bool span(int *x0, int *y0, int *x1, int *y1, int x, int y, int width,
                 int height, struct ui_rect clip)
{
    *x0 = MAX(MAX(x, clip.x), 0);
    *y0 = MAX(MAX(y, clip.y), 0);
    *x1 = MIN(MIN(x + width, clip.x + clip.width), LCD_WIDTH);
    *y1 = MIN(MIN(y + height, clip.y + clip.height), LCD_HEIGHT);
    return *x0 < *x1 && *y0 < *y1;
}

static void put(struct stock_canvas *c, const struct stock_art *art,
                int sx, int sy, int width, int height, int x, int y,
                bool mask, uint32_t rgb)
{
    int x0, y0, x1, y1;
    if (!stock_art_ready() ||
        !span(&x0, &y0, &x1, &y1, x, y, width, height,
              (struct ui_rect){ c->x, c->y, c->width, c->height }))
        return;
    for (int row = y0; row < y1; row++)
    {
        uint32_t *out = c->pixels + (row - c->y) * c->width + (x0 - c->x);
        int at = (row - y + sy) * art->width + (x0 - x + sx);
        for (int col = x0; col < x1; col++, out++, at++)
        {
            uint32_t t = texel(art, at);
            unsigned a = t >> 24;
            if (a)
                *out = blend(*out, mask ? rgb : t, a);
        }
    }
}

void stock_art_put(struct stock_canvas *canvas, enum stock_art_id id,
                   int x, int y)
{
    const struct stock_art *art = &stock_art[id];
    put(canvas, art, 0, 0, art->width, art->height, x, y, false, 0);
}

void stock_art_put_part(struct stock_canvas *canvas, enum stock_art_id id,
                        int sx, int sy, int width, int height, int x, int y)
{
    put(canvas, &stock_art[id], sx, sy, width, height, x, y, false, 0);
}

void stock_art_mask(struct stock_canvas *canvas, enum stock_art_id id,
                    int x, int y, uint32_t rgb)
{
    const struct stock_art *art = &stock_art[id];
    put(canvas, art, 0, 0, art->width, art->height, x, y, true, rgb);
}

void stock_canvas_blend(struct stock_canvas *canvas, int x, int y,
                        uint32_t rgb, unsigned alpha)
{
    x -= canvas->x;
    y -= canvas->y;
    if (alpha && x >= 0 && y >= 0 && x < canvas->width && y < canvas->height)
    {
        uint32_t *p = canvas->pixels + y * canvas->width + x;
        *p = blend(*p, rgb, alpha);
    }
}

/* d * (255 - a) / 255 + s * a / 255, rounded down. */
static inline unsigned mix255(unsigned d, unsigned s, unsigned a)
{
    return ((d * (255 - a) + s * a + 1) * 257) >> 16;
}

void stock_canvas_text(struct stock_canvas *canvas, int font, const char *text,
                       int x, int y, uint32_t rgb)
{
    if (font_prefetch_active() && !font_prefetch_text(font, text))
        return;
    font_lock(font, true);
    struct font *pf = font_get(font);
    struct font_run run = { 0 };
    for (const ucschar_t *ucs = bidi_l2v((const unsigned char *)text, 1);
         pf->pbf.file_size && ucs && *ucs; ucs++)
    {
        struct font_glyph g;
        font_run_next(pf, &run, *ucs, &g);
        if (!g.bits)
            continue;
        for (int row = 0; row < g.height; row++)
        {
            int py = y + g.y + row - canvas->y;
            if (py < 0 || py >= canvas->height)
                continue;
            for (int col = 0; col < g.width; col++)
            {
                int px = x + g.x + col - canvas->x, i = row * g.width + col;
                /* A 4-bit glyph packs its pixels low nibble first, 15 clear. */
                unsigned a = g.coverage8 ? g.bits[i] :
                    (15 - ((g.bits[i >> 1] >> (i & 1 ? 4 : 0)) & 15)) * 17;
                if (!a || px < 0 || px >= canvas->width)
                    continue;
                uint32_t *p = canvas->pixels + py * canvas->width + px;
                *p = mix255(*p >> 16 & 255, rgb >> 16 & 255, a) << 16 |
                     mix255(*p >> 8 & 255, rgb >> 8 & 255, a) << 8 |
                     mix255(*p & 255, rgb & 255, a);
            }
        }
    }
    font_lock(font, false);
}

void stock_canvas_show(const struct stock_canvas *canvas, struct ui_rect clip)
{
    int x0, y0, x1, y1;
    if (!span(&x0, &y0, &x1, &y1, canvas->x, canvas->y, canvas->width,
              canvas->height, clip))
        return;
    for (int row = y0; row < y1; row++)
    {
        const uint32_t *in = canvas->pixels + (row - canvas->y) * canvas->width +
                             (x0 - canvas->x);
        fb_data *out = lcd_framebuffer_default.fb_ptr + row * LCD_WIDTH + x0;
        for (int col = x0; col < x1; col++)
            *out++ = narrow(*in++);
    }
}

/* The LCD controller's blend of the UI layer's `s` over the picture
 * layer's RGB565 `d`, which it widens with zero low bits. */
static inline fb_data blend_layer(fb_data d, uint32_t s, unsigned a)
{
    return FB_RGBPACK(mix255(FB_UNPACK_RED(d) & 0xf8, (s >> 16) & 255, a),
                      mix255(FB_UNPACK_GREEN(d) & 0xfc, (s >> 8) & 255, a),
                      mix255(FB_UNPACK_BLUE(d) & 0xf8, s & 255, a));
}

/* Blend `count` texels of `art` from index `at` over the 565 pixels at
 * `out`: each pixel is widened to 8 bits, blended and truncated. */
static void blend_span(const struct stock_art *art, int at, fb_data *out,
                       int count, bool mask, uint32_t rgb)
{
    for (; count > 0; count--, out++, at++)
    {
        uint32_t t = texel(art, at);
        unsigned a = t >> 24;
        if (a == 255 && !mask)
            *out = narrow(t);
        else if (a && over_layer)
            *out = blend_layer(*out, mask ? rgb : t, a);
        else if (a)
            *out = narrow(blend(widen(*out), mask ? rgb : t, a));
    }
}

void stock_art_over_layer(bool on)
{
    over_layer = on;
}

static void draw(enum stock_art_id id, int x, int y, bool mask, uint32_t rgb,
                 struct ui_rect clip)
{
    const struct stock_art *art = &stock_art[id];
    int x0, y0, x1, y1;
    if (!stock_art_ready() ||
        !span(&x0, &y0, &x1, &y1, x, y, art->width, art->height, clip))
        return;
    for (int row = y0; row < y1; row++)
        blend_span(art, (row - y) * art->width + (x0 - x),
                   lcd_framebuffer_default.fb_ptr + row * LCD_WIDTH + x0,
                   x1 - x0, mask, rgb);
}

void stock_art_row(enum stock_art_id id, int sx, int sy, fb_data *out,
                   int count)
{
    const struct stock_art *art = &stock_art[id];
    if (stock_art_ready())
        blend_span(art, sy * art->width + sx, out, count, false, 0);
}

void stock_art_draw(enum stock_art_id id, int x, int y, struct ui_rect clip)
{
    draw(id, x, y, false, 0, clip);
}

void stock_art_draw_mask(enum stock_art_id id, int x, int y, uint32_t rgb,
                         struct ui_rect clip)
{
    draw(id, x, y, true, rgb, clip);
}
