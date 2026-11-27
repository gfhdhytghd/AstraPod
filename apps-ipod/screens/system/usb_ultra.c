/* PodBox Fruit Company Ultra: the USB screen's entry. GPL version 2+.
 *
 * Once the host is answered, the sphere and its connector grow into place
 * on a spring from 55% of their size, fading in over the first frames, and
 * as they settle a glint crosses them once. The backdrop and the picture
 * are kept at RGB565 in a transient surface; the last frame writes them
 * back unchanged, so the screen at rest is Fruit Company's. */
#include <string.h>

#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"

#include "ui/animation.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/perf.h"
#include "ui/ultra.h"
#include "usb_ultra.h"

#define MARGIN 4                /* room for the spring's overshoot */
#define SMALLEST (UI_ULTRA_ONE * 55 / 100)
#define GROW_FRAMES ANIM_F(30)
#define FADE_FRAMES ANIM_F(12)
#define GLINT_START ANIM_F(18)
#define GLINT_FRAMES ANIM_F(30)
#define GLINT_HALF 24
#define GLINT_PEAK 11           /* of 32 */
/* How far a pixel's red and green, in 565 steps, stand off the backdrop for
 * the glint to light it: the sphere's soft shadow stays dark. */
#define GLINT_INK 12
#define SPREAD_MASK 0x07e0f81fu

static int handle;
static struct ui_rect area, sprite;

static uint32_t spread(fb_data pixel)
{
    return (pixel | (uint32_t)pixel << 16) & SPREAD_MASK;
}

static fb_data pack(uint32_t c)
{
    c &= SPREAD_MASK;
    return c | c >> 16;
}

static int distance(int a, int b)
{
    return a < b ? b - a : a - b;
}

static fb_data *screen(int x, int y)
{
    return lcd_framebuffer_default.fb_ptr + y * LCD_WIDTH + x;
}

static void copy(fb_data *pixels, struct ui_rect r, bool in)
{
    for (int y = 0; y < r.height; ++y)
    {
        if (in)
            memcpy(pixels + y * r.width, screen(r.x, r.y + y),
                   r.width * sizeof(fb_data));
        else
            memcpy(screen(r.x, r.y + y), pixels + y * r.width,
                   r.width * sizeof(fb_data));
    }
}

bool usb_ultra_open(struct ui_rect picture)
{
    usb_ultra_close();
    if (!ui_theme_ultra())
        return false;
    sprite = picture;
    area = (struct ui_rect){ picture.x - MARGIN, picture.y - MARGIN,
                             picture.width + 2 * MARGIN,
                             picture.height + 2 * MARGIN };
    size_t pixels = (size_t)sprite.width * sprite.height;
    handle = core_alloc_free_space(((size_t)area.width * area.height +
                                    pixels) * sizeof(fb_data) + pixels);
    if (handle <= 0)
        handle = 0;
    return handle > 0;
}

void usb_ultra_keep_backdrop(void)
{
    if (!handle)
        return;
    fb_data *pixels = core_get_data_pinned(handle);
    copy(pixels, area, true);
    core_put_data_pinned(pixels);
}

/* What each picture pixel is: backdrop, picture, or picture the glint
 * lights. */
enum { PLAIN, DRAWN, LIT };

static void classify(uint8_t *kind, const fb_data *back,
                     const fb_data *picture)
{
    for (int y = 0; y < sprite.height; ++y)
    {
        const fb_data *plain = back + (sprite.y - area.y + y) * area.width +
                               sprite.x - area.x;
        for (int x = 0; x < sprite.width; ++x)
        {
            fb_data p = *picture++, q = plain[x];
            int ink = distance(p >> 11, q >> 11) +
                      distance((p >> 5) & 63, (q >> 5) & 63);
            *kind++ = p == q ? PLAIN : ink >= GLINT_INK ? LIT : DRAWN;
        }
    }
}

/* One frame: the picture at `scale` (Q10) about its centre, weighing
 * `alpha` of 32 over the backdrop, with the glint's diagonal at `glint`. */
static void compose(const fb_data *back, const fb_data *picture,
                    const uint8_t *kinds, int scale, unsigned alpha,
                    int glint)
{
    int cx = sprite.x * 2 + sprite.width, cy = sprite.y * 2 + sprite.height;
    int inverse = (UI_ULTRA_ONE << 10) / scale;
    int16_t column[LCD_WIDTH];
    uint8_t lit[GLINT_HALF + 1];
    for (int d = 0; d <= GLINT_HALF; ++d)
        lit[d] = d * d * GLINT_PEAK / (GLINT_HALF * GLINT_HALF);
    for (int x = 0; x < area.width; ++x)
        column[x] = (((area.x + x) * 2 + 1 - cx) * inverse >> 11) +
                    cx / 2 - sprite.x;
    for (int y = 0; y < area.height; ++y)
    {
        const fb_data *under = back + y * area.width;
        fb_data *out = screen(area.x, area.y + y);
        int sy = (((area.y + y) * 2 + 1 - cy) * inverse >> 11) +
                 cy / 2 - sprite.y;
        if (sy < 0 || sy >= sprite.height)
        {
            memcpy(out, under, area.width * sizeof(fb_data));
            continue;
        }
        const fb_data *source = picture + sy * sprite.width;
        const uint8_t *kind = kinds + sy * sprite.width;
        for (int x = 0; x < area.width; ++x)
        {
            int sx = column[x];
            fb_data pixel = under[x];
            if (sx >= 0 && sx < sprite.width && kind[sx] != PLAIN)
            {
                uint32_t a = spread(pixel), b = spread(source[sx]);
                uint32_t c = (a * (32 - alpha) + b * alpha) >> 5;
                int d = sx + sy - glint;
                d = GLINT_HALF - (d < 0 ? -d : d);
                if (d > 0 && kind[sx] == LIT)
                {
                    c &= SPREAD_MASK;
                    c += (spread(0xffff) - c) * lit[d] >> 5;
                }
                pixel = pack(c);
            }
            out[x] = pixel;
        }
    }
}

void usb_ultra_enter(void (*draw)(int top, int bottom))
{
    if (!handle)
        return;
    fb_data *back = core_get_data_pinned(handle);
    fb_data *picture = back + area.width * area.height;
    uint8_t *kinds = (uint8_t *)(picture + sprite.width * sprite.height);
    draw(sprite.y, sprite.y + sprite.height);
    copy(picture, sprite, true);
    copy(back, area, false);
    classify(kinds, back, picture);

    unsigned trace = ui_perf_begin("usb-screen");
    cpu_boost(true);
    uint32_t start = ui_clock_start();
    const unsigned frames = GLINT_START + GLINT_FRAMES;
    for (;;)
    {
        uint32_t elapsed = USEC_TIMER - start;
        unsigned frame = ui_clock_frame(elapsed);
        if (frame >= frames || !lcd_active())
            break;
        uint32_t begun = USEC_TIMER;
        unsigned grow = MIN(frame, GROW_FRAMES) * UI_ULTRA_ONE / GROW_FRAMES;
        int scale = SMALLEST + (UI_ULTRA_ONE - SMALLEST) *
                    ui_ultra_spring(grow) / UI_ULTRA_ONE;
        unsigned alpha = frame >= FADE_FRAMES ? 32 :
            (ui_ultra_out(frame * UI_ULTRA_ONE / FADE_FRAMES) + 16) >> 5;
        int glint = INT16_MIN;
        if (frame >= GLINT_START)
        {
            /* Across the disc's diagonal extent, 0.65 of its box, with
             * the band's half width either side. */
            int size = sprite.width + sprite.height;
            int from = size * 18 / 100 - GLINT_HALF;
            int to = size * 82 / 100 + GLINT_HALF;
            glint = from + (to - from) * (int)(frame - GLINT_START) /
                           GLINT_FRAMES;
        }
        compose(back, picture, kinds, scale, alpha, glint);
        ui_note_render((uint32_t)USEC_TIMER - begun);
        ui_mark_dirty_rect(SCREEN_MAIN, area.x, area.y, area.width,
                           area.height);
        ui_flush_dirty();
        /* Every refresh: the work is small, and sleeping to the next one
         * lets the USB thread answer the host meanwhile. */
        ui_clock_sleep_until(start + ui_frame_clock(frame + 1));
    }
    copy(back, area, false);
    copy(picture, sprite, false);
    ui_mark_dirty_rect(SCREEN_MAIN, area.x, area.y, area.width, area.height);
    ui_flush_dirty();
    cpu_boost(false);
    ui_perf_end(trace);
    core_put_data_pinned(back);
    usb_ultra_close();
}

void usb_ultra_close(void)
{
    if (handle > 0)
        core_free(handle);
    handle = 0;
}
