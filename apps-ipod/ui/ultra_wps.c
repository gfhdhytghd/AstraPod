/* PodBox Fruit Company Ultra: Now Playing motion. GPL version 2+.
 *
 * Now Playing at rest draws exactly as Fruit Company's; this file moves it
 * only while something changes:
 *   track change   the title block dissolves into the next track's, each
 *                  line that changed sliding in from the right, and a new
 *                  cover dissolves into place;
 *   progress fill  drains back on a track change and glides after a seek;
 *                  a soft gloss travels along it once the fill has drained
 *                  and as a seek lands;
 *   volume row     slides in and out with an overshoot.
 * The dissolves keep both sides in a transient surface and blend them into
 * the frame buffer, two RGB565 channels spread over one 32-bit word. */
#include <limits.h>
#include <string.h>

#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"

#include "frame.h"
#include "perf.h"
#include "ultra.h"
#include "ultra_wps.h"

#define FILL_TRACK_FRAMES ANIM_F(20)
#define FILL_SEEK_FRAMES ANIM_F(8)
#define FILL_LAND_FRAMES ANIM_F(12)
#define ROW_IN_FRAMES ANIM_F(26)
#define ROW_OUT_FRAMES ANIM_F(22)
#define GLOSS_HALF 18
#define GLOSS_FRAMES ANIM_F(34)
#define GLOSS_PEAK 18           /* of 32, at the bar's upper rows */
#define SPREAD_MASK 0x07e0f81fu

static fb_data *screen_row(int y)
{
    return lcd_framebuffer_default.fb_ptr + y * LCD_WIDTH;
}

static uint32_t spread(fb_data pixel)
{
    return (pixel | (uint32_t)pixel << 16) & SPREAD_MASK;
}

static fb_data pack(uint32_t c)
{
    return c | c >> 16;
}

static int curve_at(uint8_t curve, unsigned u)
{
    if (u >= UI_ULTRA_ONE)
        return UI_ULTRA_ONE;
    switch (curve)
    {
    case ULTRA_BACK:
        return ui_ultra_back(u);
    case ULTRA_SPRING:
        return ui_ultra_spring(u);
    case ULTRA_SMOOTH:
        return (int)(u * u / UI_ULTRA_ONE * (3 * UI_ULTRA_ONE - 2 * u) /
                     UI_ULTRA_ONE);
    default:
        return ui_ultra_out(u);
    }
}

/* Progress of frame `frame` of `frames`, counting the first shown frame as
 * a step already taken. */
static unsigned progress(unsigned frame, unsigned frames)
{
    return frame + 1 >= frames ? UI_ULTRA_ONE
                               : (frame + 1) * UI_ULTRA_ONE / frames;
}

static void trace(struct ultra_wps *u, const char *route)
{
    if (!u->trace)
        u->trace = ui_perf_begin(route);
}

void ultra_wps_glide(struct ultra_glide *glide, struct ui_page *page,
                     struct ui_view *view, int property, int from, int to,
                     unsigned frames, enum ultra_curve curve, uint32_t now)
{
    *glide = (struct ultra_glide){
        .view = view, .start = now, .from = from, .to = to,
        .property = property, .frames = frames, .curve = curve,
        .active = from != to && frames,
    };
    if (!glide->active)
        ui_page_set(page, view, property, to);
}

static bool glide_step(struct ultra_glide *glide, struct ui_page *page,
                       uint32_t now)
{
    if (!glide->active)
        return false;
    unsigned frame = ui_clock_frame(now - glide->start);
    int value = ui_ultra_mix(glide->from, glide->to,
                             curve_at(glide->curve,
                                      frame * UI_ULTRA_ONE / glide->frames));
    ui_page_set(page, glide->view, glide->property, value);
    glide->active = frame < glide->frames;
    return !glide->active;
}

void ultra_wps_row(struct ultra_wps *u, struct ui_page *page,
                   struct ui_view *row, bool in, uint32_t now)
{
    trace(u, "wps-volume");
    ultra_wps_glide(&u->row, page, row, UI_OFFSET_X, 0, -LCD_WIDTH,
                    in ? ROW_IN_FRAMES : ROW_OUT_FRAMES, ULTRA_BACK, now);
}

static void gloss_begin(struct ultra_wps *u, int width, uint32_t now)
{
    u->gloss_pending = false;
    if (width <= GLOSS_HALF)
        return;
    trace(u, "wps-gloss");
    u->gloss = true;
    u->gloss_start = now;
    u->gloss_x = INT_MIN / 2;
    u->gloss_width = width;
}

void ultra_wps_fill(struct ultra_wps *u, struct ui_page *page,
                    struct ui_view *fill, int width, enum ultra_fill why,
                    uint32_t now)
{
    struct ultra_glide *glide = &u->fill;
    int shown = fill->value[UI_WIDTH];
    glide->view = fill;
    if (why == ULTRA_FILL_PLAY && glide->active)
        glide->to = width;
    else if (why == ULTRA_FILL_PLAY || !u->live || shown == width)
    {
        glide->active = false;
        ui_page_set(page, fill, UI_WIDTH, width);
    }
    else
    {
        bool track = why == ULTRA_FILL_TRACK;
        trace(u, track ? "wps-track-progress" : "wps-seek-progress");
        ultra_wps_glide(glide, page, fill, UI_WIDTH, shown, width,
                        track ? FILL_TRACK_FRAMES :
                        why == ULTRA_FILL_SEEK ? FILL_SEEK_FRAMES :
                        FILL_LAND_FRAMES,
                        why == ULTRA_FILL_LAND ? ULTRA_BACK : ULTRA_OUT, now);
    }
    if (why == ULTRA_FILL_TRACK)
        u->gloss_pending = u->live;
    else if (why == ULTRA_FILL_LAND && u->live)
        gloss_begin(u, width, now);
}

static struct ui_rect gloss_rect(const struct ultra_wps *u)
{
    struct ui_rect box = u->gloss_box;
    int left = MAX(u->gloss_x - GLOSS_HALF, box.x);
    int right = MIN(u->gloss_x + GLOSS_HALF, box.x + u->gloss_width);
    return right > left ? (struct ui_rect){ left, box.y, right - left,
                                            box.height }
                        : (struct ui_rect){ 0 };
}

static void gloss_step(struct ultra_wps *u, struct ui_page *page,
                       uint32_t now)
{
    struct ui_view *fill = u->fill.view;
    if (u->gloss_pending && !u->fill.active && fill)
        gloss_begin(u, fill->value[UI_WIDTH], now);
    if (!u->gloss)
        return;
    struct ui_rect before = gloss_rect(u);
    int x = u->gloss_x, width = u->gloss_width;
    unsigned frame = ui_clock_frame(now - u->gloss_start);
    int from = u->gloss_box.x - GLOSS_HALF;
    int to = u->gloss_box.x + u->gloss_width + GLOSS_HALF;
    u->gloss_x = ui_ultra_mix(from, to,
        curve_at(ULTRA_SMOOTH, frame * UI_ULTRA_ONE / GLOSS_FRAMES));
    u->gloss_width = fill->value[UI_WIDTH];
    if (frame >= GLOSS_FRAMES)
        u->gloss = false;
    struct ui_rect after = gloss_rect(u);
    /* An unchanged gloss sends nothing, keeping its refresh for the next
     * frame. */
    if (u->gloss && u->gloss_x == x && u->gloss_width == width)
        return;
    if (before.width)
        ui_page_invalidate(page, before);
    if (after.width && u->gloss)
        ui_page_invalidate(page, after);
}

void ultra_wps_paint_gloss(const struct ultra_wps *u, struct ui_rect bar,
                           struct ui_rect clip)
{
    if (!u->gloss)
        return;
    struct ui_rect box = gloss_rect(u);
    int x0 = MAX(MAX(box.x, clip.x), bar.x);
    int x1 = MIN(MIN(box.x + box.width, clip.x + clip.width),
                 bar.x + bar.width);
    int y0 = MAX(clip.y, bar.y);
    int y1 = MIN(clip.y + clip.height, bar.y + bar.height);
    if (x1 <= x0 || y1 <= y0)
        return;
    /* Column weights fall off as a squared hat; rows fade down the bar and
     * its reflection, which starts CLASSIC's 15 rows down. */
    uint8_t column[2 * GLOSS_HALF];
    for (int x = x0; x < x1; ++x)
    {
        int dx = x - u->gloss_x;
        int d = GLOSS_HALF - (dx < 0 ? -dx : dx);
        column[x - x0] = d > 0 ? d * d * GLOSS_PEAK /
                                 (GLOSS_HALF * GLOSS_HALF) : 0;
    }
    const uint32_t white = spread(0xffff);
    for (int y = y0; y < y1; ++y)
    {
        int row = y - bar.y;
        int shade = row < 15 ? 32 - row : row < 26 ? (26 - row) * 3 / 2 : 0;
        if (shade <= 0)
            continue;
        fb_data *dst = screen_row(y) + x0;
        for (int x = 0; x < x1 - x0; ++x)
        {
            unsigned a = column[x] * shade >> 5;
            if (!a)
                continue;
            uint32_t c = spread(dst[x]);
            c = (c + ((white - c) * a >> 5)) & SPREAD_MASK;
            dst[x] = pack(c);
        }
    }
}

static void boost(struct ultra_wps *u, bool on)
{
    if (u->boosted != on)
    {
        u->boosted = on;
        cpu_boost(on);
    }
}

static void fade_end(struct ultra_fade *fade)
{
    if (fade->handle > 0)
        fade->handle = core_free(fade->handle);
    fade->handle = 0;
    fade->active = false;
}

static void copy_area(fb_data *dst, const struct ui_rect *area, bool in)
{
    for (int y = 0; y < area->height; ++y)
    {
        fb_data *screen = screen_row(area->y + y) + area->x;
        fb_data *pixels = dst + y * area->width;
        if (in)
            memcpy(pixels, screen, area->width * sizeof(fb_data));
        else
            memcpy(screen, pixels, area->width * sizeof(fb_data));
    }
}

void ultra_wps_fade(struct ultra_wps *u, enum ultra_fade_slot slot,
                    struct ui_rect area, struct ui_rect skip, int slide,
                    unsigned frames)
{
    struct ultra_fade *fade = &u->fade[slot];
    size_t pixels = (size_t)area.width * area.height;
    if (!fade->active)
    {
        fade->handle = core_alloc_free_space(2 * pixels * sizeof(fb_data));
        if (fade->handle <= 0)
        {
            fade->handle = 0;
            return;
        }
    }
    fb_data *old = core_get_data_pinned(fade->handle);
    copy_area(old, &area, true);
    /* A dissolve in progress restarts from what it shows; the page paints
     * over the true pixels under it. */
    if (fade->active && !fade->fresh)
        copy_area(old + pixels, &area, false);
    core_put_data_pinned(old);
    *fade = (struct ultra_fade){
        .area = area, .skip = skip, .handle = fade->handle, .shown = -1,
        .slide = slide, .frames = frames, .active = true, .fresh = true,
    };
    trace(u, slot == ULTRA_FADE_TEXT ? "wps-track-fade" : "wps-cover-fade");
    boost(u, true);
}

void ultra_wps_step(struct ultra_wps *u, struct ui_page *page, uint32_t now)
{
    if (glide_step(&u->row, page, now))
        u->row.view = NULL;
    glide_step(&u->fill, page, now);
    gloss_step(u, page, now);
}

static bool overlaps(struct ui_rect a, struct ui_rect b)
{
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 &&
           a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

/* dst = old weighing 32 - w over new weighing w. */
static void mix_row(fb_data *dst, const fb_data *old, const fb_data *new,
                    int count, unsigned w)
{
    for (int x = 0; x < count; ++x)
    {
        uint32_t c = (spread(old[x]) * (32 - w) + spread(new[x]) * w) >> 5;
        dst[x] = pack(c & SPREAD_MASK);
    }
}

static inline fb_data blend(uint32_t base, uint32_t a, unsigned wa,
                            uint32_t b, unsigned wb)
{
    return pack(((base + a * wa + b * wb) >> 5) & SPREAD_MASK);
}

/* A changed line: the old row moved `out` pixels left weighing `wa`, the
 * new one `in` pixels right weighing `wb`, over `paper`, which also fills
 * what either uncovers. Out of line, so compose()'s locals leave its loop
 * the registers. */
static NO_INLINE void slide_row(fb_data *dst, const fb_data *old,
                                const fb_data *new, int count, unsigned wa,
                                unsigned wb, int out, int in, fb_data paper)
{
    uint32_t blank = spread(paper);
    uint32_t base = blank * (32 - wa - wb);
    /* The new row covers from `in` and the old one up to `count - out`;
     * nearly every pixel lies under both. */
    int x = 0, from = MIN(in, count), to = MAX(count - out, from);
    for (; x < from; ++x)
    {
        uint32_t a = x + out < count ? spread(old[x + out]) : blank;
        dst[x] = blend(base, a, wa, blank, wb);
    }
    for (; x < to; ++x)
        dst[x] = blend(base, spread(old[x + out]), wa, spread(new[x - in]), wb);
    for (; x < count; ++x)
        dst[x] = blend(base, blank, wa, spread(new[x - in]), wb);
}

static bool row_blank(const fb_data *row, int count, fb_data paper)
{
    for (int x = 0; x < count; ++x)
        if (row[x] != paper)
            return false;
    return true;
}

static void compose(struct ultra_fade *fade, const fb_data *old,
                    const fb_data *new, unsigned frame)
{
    const struct ui_rect *a = &fade->area;
    unsigned t = progress(frame, fade->frames);
    unsigned w = (curve_at(ULTRA_SMOOTH, t) + 16) >> 5;
    /* A changed line leaves over the first 55% and the next one arrives
     * over the last 70%, so the two cross only briefly. */
    unsigned leave = MIN(UI_ULTRA_ONE, t * 20 / 11);
    unsigned arrive = t <= UI_ULTRA_ONE * 3 / 10 ? 0 :
                      (t - UI_ULTRA_ONE * 3 / 10) * 10 / 7;
    unsigned wb = (curve_at(ULTRA_SMOOTH, arrive) + 16) >> 5;
    unsigned wa = (32 - ((curve_at(ULTRA_SMOOTH, leave) + 16) >> 5)) *
                  (32 - wb) >> 5;
    int out = fade->slide * curve_at(ULTRA_OUT, leave) / UI_ULTRA_ONE;
    int travel = fade->slide * (UI_ULTRA_ONE - curve_at(ULTRA_OUT, arrive)) /
                 UI_ULTRA_ONE;
    int skip_left = fade->skip.x - a->x;
    int skip_right = skip_left + fade->skip.width;
    for (int y = 0; y < a->height; )
    {
        /* A band runs from a row either side shows to the next blank one;
         * one that differs anywhere slides as a whole. */
        int end = y + 1;
        bool moved = false;
        fb_data paper = a->x > 0 ? screen_row(a->y + y)[a->x - 1] : 0;
        if (fade->slide)
        {
            while (end <= a->height)
            {
                const fb_data *o = old + (end - 1) * a->width;
                const fb_data *n = new + (end - 1) * a->width;
                if (row_blank(o, a->width, paper) &&
                    row_blank(n, a->width, paper))
                    break;
                moved |= memcmp(o, n, a->width * sizeof(fb_data)) != 0;
                ++end;
            }
            end = MIN(MAX(end, y + 1), a->height);
        }
        for (int row = y; row < end; ++row)
        {
            fb_data *dst = screen_row(a->y + row) + a->x;
            const fb_data *o = old + row * a->width;
            const fb_data *n = new + row * a->width;
            int sy = a->y + row;
            bool skipped = sy >= fade->skip.y &&
                           sy < fade->skip.y + fade->skip.height &&
                           fade->skip.width > 0;
            if (moved)
                slide_row(dst, o, n, a->width, wa, wb, out, travel,
                          paper);
            else if (!skipped)
                mix_row(dst, o, n, a->width, w);
            else
            {
                mix_row(dst, o, n, MAX(skip_left, 0), w);
                if (skip_right < a->width)
                    mix_row(dst + skip_right, o + skip_right,
                            n + skip_right, a->width - skip_right, w);
            }
        }
        y = end;
    }
}

void ultra_wps_draw(struct ultra_wps *u, struct ui_page *page, bool submit)
{
    uint32_t now = USEC_TIMER;
    bool due[ULTRA_FADE_COUNT];
    for (int i = 0; i < ULTRA_FADE_COUNT; ++i)
    {
        struct ultra_fade *fade = &u->fade[i];
        due[i] = false;
        if (!fade->active)
            continue;
        if (!submit)
        {
            if (!fade->fresh)
            {
                fb_data *pixels = core_get_data_pinned(fade->handle);
                copy_area(pixels + fade->area.width * fade->area.height,
                          &fade->area, false);
                core_put_data_pinned(pixels);
            }
            fade_end(fade);
            continue;
        }
        due[i] = fade->fresh || overlaps(page->damage, fade->area) ||
                 (int)ui_clock_frame(now - fade->start) != fade->shown;
        if (due[i] && !fade->fresh)
        {
            fb_data *pixels = core_get_data_pinned(fade->handle);
            copy_area(pixels + fade->area.width * fade->area.height,
                      &fade->area, false);
            core_put_data_pinned(pixels);
        }
    }

    struct ui_rect damage = ui_page_paint(page);
    if (submit)
        u->live = true;
    uint32_t start = USEC_TIMER;
    bool composed = false;
    for (int i = 0; i < ULTRA_FADE_COUNT; ++i)
    {
        struct ultra_fade *fade = &u->fade[i];
        if (!due[i])
            continue;
        size_t pixels = (size_t)fade->area.width * fade->area.height;
        fb_data *old = core_get_data_pinned(fade->handle);
        fb_data *new = old + pixels;
        copy_area(new, &fade->area, true);
        if (fade->fresh)
        {
            fade->fresh = false;
            fade->start = ui_clock_start();
            if (!memcmp(old, new, pixels * sizeof(fb_data)))
                fade->frames = 0;
        }
        unsigned frame = ui_clock_frame(USEC_TIMER - fade->start);
        fade->shown = frame;
        if (frame + 1 < fade->frames)
            compose(fade, old, new, frame);
        core_put_data_pinned(old);
        if (frame + 1 >= fade->frames)
            fade_end(fade);
        ui_mark_dirty_rect(SCREEN_MAIN, fade->area.x, fade->area.y,
                           fade->area.width, fade->area.height);
        composed = true;
    }
    if (composed)
        ui_note_render((uint32_t)USEC_TIMER - start);
    if (!u->fade[ULTRA_FADE_TEXT].active && !u->fade[ULTRA_FADE_COVER].active)
        boost(u, false);
    if (submit)
    {
        if (damage.width > 0 && damage.height > 0)
            ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width,
                               damage.height);
        ui_flush_dirty();
    }
    if (!ultra_wps_busy(u) && u->trace)
    {
        ui_perf_end(u->trace);
        u->trace = 0;
    }
}

bool ultra_wps_busy(const struct ultra_wps *u)
{
    return u->row.active || u->fill.active || u->gloss || u->gloss_pending ||
           u->fade[ULTRA_FADE_TEXT].active || u->fade[ULTRA_FADE_COVER].active;
}

void ultra_wps_close(struct ultra_wps *u)
{
    for (int i = 0; i < ULTRA_FADE_COUNT; ++i)
        fade_end(&u->fade[i]);
    boost(u, false);
    ui_perf_end(u->trace);
    u->trace = 0;
    u->row.active = u->fill.active = u->gloss = u->gloss_pending = false;
}
