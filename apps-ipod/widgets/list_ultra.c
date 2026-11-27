/* PodBox Fruit Company Ultra list motion. GPL version 2+.
 *
 * Parts:
 *   - the spring the bar and rows glide on, with the speed they had
 *   - the glint: a slanted band of light across the settled bar
 *   - the stock marquee run a pixel a refresh, easing in and out
 *   - the scroll indicator's rise and fade
 *
 * Everything per pixel goes through shifts and one small table. */

#include "config.h"
#include "lcd.h"
#include "system.h"
#include "ui/native_theme_runtime.h"
#include "ui/ultra.h"
#include "list_ultra.h"

#define ONE UI_ULTRA_ONE
/* The spring covers most of a step in its first third and settles in
 * the rest. */
#define GLIDE_F 22
#define RETURN_F 18

unsigned list_ultra_glide_frames(void)
{
    return ui_theme_ultra() ? ui_theme_duration(GLIDE_F) : 0;
}

unsigned list_ultra_return_frames(void)
{
    return ui_theme_ultra() ? ui_theme_duration(RETURN_F) : 0;
}

/* ui_ultra_spring()'s response to a unit of starting speed, in Q14 units
 * of its duration: e^(-zwt) sin(w't) / w' for the same z and w, sampled
 * at t = i/64. */
static const short impulse[65] = {
    0, 232, 420, 567, 679, 759, 811, 840, 849, 840, 819, 786, 745, 697,
    646, 591, 536, 481, 426, 374, 323, 276, 232, 192, 155, 122, 92, 66,
    43, 23, 7, -7, -18, -27, -34, -39, -43, -45, -46, -46, -45, -44, -42,
    -39, -36, -33, -31, -27, -25, -22, -19, -16, -14, -11, -9, -7, -6, -4,
    -3, -2, -1, 0, 0,
};

int64_t list_ultra_spring(int64_t from, int64_t to, int64_t velocity,
                          unsigned frame, unsigned frames)
{
    if (frame >= frames)
        return to;
    unsigned u = frame * ONE / frames;
    unsigned i = u >> 4, frac = u & 15;
    int carry = impulse[i] + (((impulse[i + 1] - impulse[i]) * (int)frac) >> 4);
    return from + (to - from) * ui_ultra_spring(u) / ONE +
           velocity * (int64_t)frames * carry / 16384;
}

/* Smoothstep, so the return leaves from rest and arrives at rest. */
int64_t list_ultra_return(int64_t from, int64_t to, unsigned frame,
                          unsigned frames)
{
    if (frame >= frames)
        return to;
    int64_t u = frame * ONE / frames;
    int64_t weight = u * u * (3 * ONE - 2 * u) / ((int64_t)ONE * ONE);
    return from + (to - from) * weight / ONE;
}

/* Two RGB565 pixels' worth of headroom: each field of a pixel spread by
 * this mask can take a 5-bit weight without reaching the next. */
#define SPREAD 0x07e0f81fu

static inline uint32_t spread(unsigned c)
{
    return (c | ((uint32_t)c << 16)) & SPREAD;
}

static inline uint16_t gather(uint32_t v)
{
    return v | (v >> 16);
}

/* The glint's light across its width, in 32nds towards white. */
static const uint8_t glint[] = {
    0, 0, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11, 12, 12, 12, 12, 11,
    11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0,
};
#define GLINT_WIDTH ((int)sizeof(glint))

void list_ultra_glint(struct ui_rect clip, struct ui_rect bar,
                      const uint16_t *rows, unsigned frame)
{
    if (frame >= LIST_ULTRA_GLINT_FRAMES || bar.height <= 0)
        return;
    /* The band leans right by a column every two rows up. */
    int slant = bar.height / 2;
    int64_t u = frame * ONE / LIST_ULTRA_GLINT_FRAMES;
    int64_t eased = u * u * (3 * ONE - 2 * u) / ((int64_t)ONE * ONE);
    int travel = bar.width + GLINT_WIDTH + slant;
    int left = bar.x - GLINT_WIDTH - slant + (int)(travel * eased / ONE);
    int x0 = MAX(clip.x, bar.x), x1 = MIN(clip.x + clip.width,
                                          bar.x + bar.width);
    int y0 = MAX(clip.y, bar.y), y1 = MIN(clip.y + clip.height,
                                          bar.y + bar.height);
    x0 = MAX(x0, 0);
    x1 = MIN(x1, LCD_WIDTH);
    y0 = MAX(y0, 0);
    y1 = MIN(y1, LCD_HEIGHT);
    for (int y = y0; y < y1; ++y)
    {
        int start = left + (bar.y + bar.height - 1 - y) / 2;
        int a = MAX(start, x0), b = MIN(start + GLINT_WIDTH, x1);
        if (a >= b)
            continue;
        uint32_t base = spread(rows[y - bar.y]);
        uint32_t room = SPREAD - base;
        fb_data *out = FBADDRBUF((&lcd_framebuffer_default), a, y);
        const uint8_t *weight = glint + (a - start);
        for (int x = a; x < b; ++x)
            *out++ = gather(base + (((room * *weight++) >> 5) & SPREAD));
    }
}

/* Stock waits 38 ticks of 34.1 ms before the run, and 36 between runs. */
#define MARQUEE_WAIT_F 87
#define MARQUEE_REST_F 82
/* Frames the run takes to reach a column a frame, and to stop again. */
#define MARQUEE_RAMP_F 20

unsigned list_ultra_marquee(unsigned frame, unsigned period)
{
    if (frame < MARQUEE_WAIT_F || !period)
        return 0;
    unsigned run = period + MARQUEE_RAMP_F;
    unsigned at = (frame - MARQUEE_WAIT_F) % (run + MARQUEE_REST_F);
    const unsigned ramp = MARQUEE_RAMP_F;
    unsigned offset;
    if (at >= run)
        return 0;
    if (at < ramp)
        offset = at * at / (2 * ramp);
    else if (at < run - ramp)
        offset = at - ramp / 2;
    else
        offset = period - (run - at) * (run - at) / (2 * ramp);
    return offset % period;
}

unsigned list_ultra_marquee_next(unsigned frame, unsigned period)
{
    if (frame < MARQUEE_WAIT_F || !period)
        return MARQUEE_WAIT_F;
    unsigned run = period + MARQUEE_RAMP_F;
    unsigned cycle = run + MARQUEE_REST_F;
    unsigned at = (frame - MARQUEE_WAIT_F) % cycle;
    return at + 1 < run ? frame + 1 : frame + cycle - at;
}

/* The indicator rises over RISE_US when called back while fading, and
 * fades over FADE_US, as long as stock's 14 steps of 34.1 ms. */
#define RISE_US 100000
#define FADE_US 477000

int list_ultra_indicator(uint32_t moved, int from, uint32_t hold)
{
    int level = LIST_ULTRA_FULL;
    if (moved < RISE_US)
        level = from + (LIST_ULTRA_FULL - from) *
                ui_ultra_out(moved * (ONE / 8) / (RISE_US / 8)) / ONE;
    if (moved < hold)
        return level;
    if (moved - hold >= FADE_US)
        return 0;
    int64_t u = (int64_t)(moved - hold) * ONE / FADE_US;
    int64_t eased = u * u * (3 * ONE - 2 * u) / ((int64_t)ONE * ONE);
    return MIN(level, (int)(LIST_ULTRA_FULL * (ONE - eased) / ONE));
}

bool list_ultra_indicator_moving(uint32_t moved, int from, uint32_t hold)
{
    return (moved < RISE_US && from < LIST_ULTRA_FULL) ||
           (moved >= hold && moved - hold < FADE_US);
}

void list_ultra_mix_row(uint16_t *dst, const uint16_t *src, int width,
                        int level)
{
    unsigned weight = (MIN(MAX(level, 0), LIST_ULTRA_FULL) + 4) >> 3;
    for (int i = 0; i < width; ++i)
    {
        uint32_t under = spread(dst[i]), over = spread(src[i]);
        dst[i] = gather((under + (((over - under) * weight) >> 5)) & SPREAD);
    }
}
