/* PodBox scene transitions. GPL version 2+. */
#include "config.h"
#include "plugins/rock_compat.h"
#include "button.h"
#include "core_alloc.h"
#include "draw/viewport.h"
#include "kernel.h"
#include "lcd.h"
#include "memset16.h"
#include "system.h"
#include "thread.h"
#include "frame.h"
#include "menu_pane.h"
#include "native_theme_runtime.h"
#include "perf.h"
#include "retained.h"
#include "scene.h"
#include "statusbar.h"
#include <string.h>

#define PAGE_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define SCENE_BYTES (3 * FRAMEBUFFER_SIZE)
#define PANEL_WORKSPACE_BYTES (2 * FRAMEBUFFER_SIZE)
#define PUSH_ROW_FRAMES ANIM_F(12)
#define PUSH_ROW_DELAY_HALF 3
#define PUSH_ROW_FADE ANIM_F(6)
#define PUSH_ROW_OFFSET (LCD_WIDTH * 60 / 100)
#define POP_FRAMES ANIM_F(15)
#define PAN_FRAMES ANIM_F(12)
#define PARALLAX_PERCENT 30
#define COVERED_BRIGHTNESS 179
#define MODAL_UP_FRAMES ANIM_F(16)
#define MODAL_DOWN_FRAMES ANIM_F(12)
#define MODAL_DIM (UI_FULL_OPACITY * 35 / 100)

static struct ui_scene *owner;
static unsigned int owner_thread;
static const void *root_page, *restored_page;
static bool (*parent_painter)(const void *page);

size_t ui_scene_reserve_size(void)
{
    /* One settled backdrop under a nested panel that is still moving. */
    return 4 * FRAMEBUFFER_SIZE + 3 * (BUFLIB_ALLOC_OVERHEAD + sizeof(intptr_t));
}

static fb_data *surface(fb_data *data, bool front)
{
    return data + (front ? PAGE_PIXELS : 0);
}

static int position(const struct ui_scene *scene, unsigned frame)
{
    const struct ui_tween *tween = &scene->tween;
    int offset;
    if (scene->pan_top && !scene->slide && frame < tween->frames)
    {
        /* Smoothstep: zero velocity at each end, equal travel in each half. */
        int t = frame * 128 / tween->frames;
        int weight = t * t * (384 - 2 * t);
        offset = tween->from + (tween->to - tween->from) *
                               weight / (128 * 128 * 128);
    }
    else
        offset = ui_tween_sample(tween, frame);
    offset &= ~1;
    /* Keep the last two-pixel strip until the endpoint is due. */
    if (frame < tween->frames && tween->from != tween->to && offset == tween->to)
        offset += tween->from > tween->to ? 2 : -2;
    return offset;
}

static void compose(fb_data *out, const fb_data *back, const fb_data *front,
                    int offset)
{
    int covered = LCD_WIDTH - offset;
    int parallax = covered * PARALLAX_PERCENT / 100;
    unsigned light = UI_FULL_OPACITY -
                     (UI_FULL_OPACITY - COVERED_BRIGHTNESS) * covered / LCD_WIDTH;
    back += parallax;
    for (int row = 0; row < LCD_HEIGHT; row++)
    {
        ui_blit_dim(out, back, offset, light);
        memcpy(out + offset, front, covered * sizeof(*out));
        back += LCD_WIDTH;
        front += LCD_WIDTH;
        out += LCD_WIDTH;
    }
}

int ui_scene_modal_body_offset(int offset, int status_height)
{
    return offset * (LCD_HEIGHT - status_height) / LCD_HEIGHT;
}

static void compose_modal(fb_data *out, const fb_data *back, const fb_data *front,
                          int offset, int status_height, fb_data tint)
{
    unsigned light = UI_FULL_OPACITY - MODAL_DIM * (LCD_HEIGHT - offset) / LCD_HEIGHT;
    if (status_height)
    {
        int chrome = status_height * LCD_WIDTH;
        memcpy(out, back, chrome * sizeof(*out));
        out += chrome;
        back += chrome;
        front += chrome;
        offset = ui_scene_modal_body_offset(offset, status_height);
    }
    ui_blit_tint(out, back, offset * LCD_WIDTH, light, tint);
    memcpy(out + offset * LCD_WIDTH, front,
           (LCD_HEIGHT - status_height - offset) * LCD_WIDTH * sizeof(*out));
}

/* out[x] = src[x - at] over [x0, x1), within the source's first `width`
 * columns. */
static void put_span(fb_data *out, const fb_data *src, int at, int width,
                     int x0, int x1)
{
    x0 = MAX(x0, at);
    x1 = MIN(x1, at + width);
    if (x1 > x0)
        memcpy(out + x0, src + (x0 - at), (x1 - x0) * sizeof(*out));
}

/* A slide with a split menu on either side. Two menus slide their rows past
 * a still pane, the arriving page's. A full page slides over the pane on the
 * way in and under it on the way out, pushing the rows aside or pulled by
 * them. A root page without menu chrome moves whole. */
static void compose_split(const struct ui_scene *scene, fb_data *out,
                          const fb_data *back, const fb_data *front, int offset)
{
    const int half = MENU_PANE_X;
    bool front_right = offset >= 0;
    const fb_data *left = front_right ? back : front;
    const fb_data *right = front_right ? front : back;
    bool left_split = front_right ? scene->back_split : scene->front_split;
    bool right_split = front_right ? scene->front_split : scene->back_split;
    bool target_right = front_right == scene->target_front;
    /* The left edge of the page on the right. */
    int p = front_right ? offset : LCD_WIDTH + offset;
    bool both = left_split && right_split && !scene->full_screen;
    int top = both ? scene->pan_top : 0;
    memcpy(out, lcd_framebuffer_default.fb_ptr, top * LCD_WIDTH * sizeof(*out));
    for (int y = top; y < LCD_HEIGHT; y++)
    {
        int i = y * LCD_WIDTH;
        fb_data *row = out + i;
        const fb_data *l = left + i, *r = right + i;
        if (scene->full_screen)
        {
            put_span(row, l, scene->back_still ? 0 : p - LCD_WIDTH, LCD_WIDTH,
                     0, LCD_WIDTH);
            put_span(row, r, p, LCD_WIDTH, 0, LCD_WIDTH);
        }
        else if (both)
        {
            /* The rows travel half as far as the pages. */
            int a = (p - LCD_WIDTH) / 2;
            put_span(row, l, a, half, 0, half);
            put_span(row, r, a + half, half, 0, half);
            menu_pane_shadow(row, y, a + half, half);
            memcpy(row + half, (target_right ? r : l) + half,
                   (LCD_WIDTH - half) * sizeof(*row));
        }
        else
        {
            const fb_data *split = left_split ? l : r;
            const fb_data *full = left_split ? r : l;
            int full_x = left_split ? p : p - LCD_WIDTH;
            int rows_x = left_split ? MIN(0, p - half) : MAX(0, p);
            bool full_over = left_split == target_right;
            memcpy(row + half, split + half, (LCD_WIDTH - half) * sizeof(*row));
            put_span(row, split, rows_x, half, 0, half);
            put_span(row, full, full_x, LCD_WIDTH, 0,
                     full_over ? LCD_WIDTH : half);
            if (!full_over && left_split)
                menu_pane_shadow(row, y, rows_x + half, half);
        }
    }
}

static void compose_pages(const struct ui_scene *scene, fb_data *out,
                          const fb_data *back, const fb_data *front, int offset)
{
    if (!scene->pan_top && !scene->slide)
    {
        compose(out, back, front, offset);
        return;
    }
    if (scene->slide && (scene->back_split || scene->front_split))
    {
        compose_split(scene, out, back, front, offset);
        return;
    }

    int chrome = scene->pan_top * LCD_WIDTH;
    memcpy(out, lcd_framebuffer_default.fb_ptr, chrome * sizeof(*out));
    int left = offset < 0 ? LCD_WIDTH + offset : offset;
    const fb_data *a = offset < 0 ? front - offset : back + LCD_WIDTH - offset;
    const fb_data *b = offset < 0 ? back : front;
    for (int row = scene->pan_top; row < LCD_HEIGHT; row++)
    {
        int i = row * LCD_WIDTH;
        memcpy(out + i, a + i, left * sizeof(*out));
        memcpy(out + i + left, b + i, (LCD_WIDTH - left) * sizeof(*out));
    }
}

static void compose_push_band(fb_data *out, const fb_data *back, const fb_data *front,
                              int lines, int age_half, int parallax, unsigned light,
                              fb_data blank)
{
    if (age_half >= 2 * PUSH_ROW_FRAMES)
    {
        memcpy(out, front, lines * LCD_WIDTH * sizeof(*out));
        return;
    }

    const struct ui_tween row = { .from = PUSH_ROW_OFFSET, .frames = 2 * PUSH_ROW_FRAMES };
    unsigned age = MAX(0, age_half);
    int dx = MAX(2, ui_tween_sample(&row, age) & ~1);
    unsigned ink = age * UI_FULL_OPACITY / (2 * PUSH_ROW_FADE);
    unsigned under = (UI_FULL_OPACITY - MIN(ink, UI_FULL_OPACITY)) * light / UI_FULL_OPACITY;
    int visible_back = LCD_WIDTH - parallax;
    for (int y = 0; y < lines; ++y)
    {
        if (!ink)
        {
            ui_blit_dim(out, back + parallax, visible_back, light);
            memset16(out + visible_back, blank, parallax);
        }
        else
        {
            ui_blit_dim(out, back + parallax, dx, light);
            if (ink >= UI_FULL_OPACITY)
                memcpy(out + dx, front, (LCD_WIDTH - dx) * sizeof(*out));
            else
            {
                ui_blit_mix(out + dx, front, back + parallax + dx,
                            visible_back - dx, ink, under);
                ui_blit_tint(out + visible_back, front + visible_back - dx,
                             parallax, ink, blank);
            }
        }
        out += LCD_WIDTH;
        back += LCD_WIDTH;
        front += LCD_WIDTH;
    }
}

static unsigned push_duration_half(const struct ui_scene *scene)
{
    int delay = scene->target_front ? PUSH_ROW_DELAY_HALF : 4;
    return 2 * PUSH_ROW_FRAMES + delay * MAX(0, scene->row_count - 1);
}

static void set_push_duration(struct ui_scene *scene)
{
    scene->push_half_frames = ui_theme_motion(UI_MOTION_PUSH,
                                              push_duration_half(scene),
                                              2 * PUSH_ROW_FRAMES);
    scene->tween.frames = (scene->push_half_frames + 1) / 2;
}

static void compose_push(const struct ui_scene *scene, fb_data *out,
                         const fb_data *back, const fb_data *front,
                         unsigned frame, int offset)
{
    int covered = LCD_WIDTH - offset;
    int parallax = covered * PARALLAX_PERCENT / 100;
    unsigned light = UI_FULL_OPACITY -
                    (UI_FULL_OPACITY - COVERED_BRIGHTNESS) * covered / LCD_WIDTH;
    fb_data bg = scene->dim_color, blank;
    ui_blit_dim(&blank, &bg, 1, light);

    int frame_in_rows = scene->target_front
        ? 2 * frame * push_duration_half(scene) / scene->push_half_frames
        : 2 * (frame * (push_duration_half(scene) / 2) / scene->tween.frames);
    int delay = scene->target_front ? PUSH_ROW_DELAY_HALF : 4;
    int y = scene->push_top;
    memcpy(out, front, y * LCD_WIDTH * sizeof(*out));
    int row_y = MAX(y, scene->row_y);
    for (int i = -1; i <= scene->row_count; ++i)
    {
        int end = i < 0 ? row_y : i == scene->row_count ?
                  LCD_HEIGHT : MIN(LCD_HEIGHT, row_y + (i + 1) * scene->row_pitch);
        int age = frame_in_rows;
        if (i > 0 && i < scene->row_count)
            age -= i * delay;
        if (end > y)
        {
            int start = y * LCD_WIDTH;
            compose_push_band(out + start, back + start, front + start, end - y,
                              age, parallax, light, blank);
            y = end;
        }
    }
}

static void complete_transition(struct ui_scene *scene)
{
    if (owner != scene)
        return;
    owner = NULL;
    lcd_hold_updates(false);
    scene->capturing = false;
    scene->automatic = false;
    scene->staggered = false;
    scene->rows_changed = false;
    scene->reuse_target = false;
    cpu_boost(false);
    ui_perf_end(scene->trace);
    scene->trace = 0;
}

static void settle_panel(struct ui_scene *scene)
{
    complete_transition(scene);
    scene->handle = core_free(scene->handle);
}

static void release_scene(struct ui_scene *scene)
{
    complete_transition(scene);
    scene->root_entry = false;
    if (scene->handle > 0)
        scene->handle = core_free(scene->handle);
    if (scene->backdrop_handle > 0)
        scene->backdrop_handle = core_free(scene->backdrop_handle);
}

static void own_transition(struct ui_scene *scene, const char *route)
{
    owner_thread = thread_self();
    owner = scene;
    scene->trace = ui_perf_begin(route);
    lcd_hold_updates(true);
    cpu_boost(true);
}

static bool present(const fb_data *pixels)
{
    uint32_t start = USEC_TIMER;
    if (!lcd_present_scene(pixels))
        return false;
    ui_note_flush((uint32_t)USEC_TIMER - start);
    return true;
}

void ui_scene_cancel(struct ui_scene *scene)
{
    if (scene->handle <= 0 && scene->backdrop_handle <= 0)
        return;
    release_scene(scene);
    struct viewport *previous = lcd_set_viewport(NULL);
    lcd_update();
    lcd_set_viewport(previous);
}

void ui_scene_discard(struct ui_scene *scene)
{
    release_scene(scene);
}

void ui_scene_interrupt(void)
{
    if (owner && thread_self() == owner_thread)
        ui_scene_cancel(owner);
}

void ui_scene_snap_push(void)
{
    if (!owner || !owner->staggered || thread_self() != owner_thread)
        return;
    if (owner->backdrop_handle > 0)
    {
        settle_panel(owner);
        struct viewport *previous = lcd_set_viewport(NULL);
        lcd_update();
        lcd_set_viewport(previous);
    }
    else
        ui_scene_cancel(owner);
}

void ui_scene_push_rows(int y, int pitch, int count)
{
    struct ui_scene *scene = owner;
    if (!scene || !(scene->staggered || scene->slide) ||
        thread_self() != owner_thread)
        return;
    y = MIN(MAX(y, 0), LCD_HEIGHT);
    count = MAX(0, count);
    if (scene->row_y == y && scene->row_pitch == pitch && scene->row_count == count)
        return;
    if (!scene->capturing &&
        ui_clock_frame((uint32_t)USEC_TIMER - scene->tween.start_clock) >= scene->tween.frames)
        return;

    scene->row_y = y;
    scene->row_pitch = pitch;
    scene->row_count = count;
    /* A loading row can become a list while the incoming page is in flight. */
    if (!scene->capturing)
    {
        if (scene->staggered)
            set_push_duration(scene);
        scene->rows_changed = true;
    }
}

void ui_scene_fade_out(void)
{
    unsigned duration = ui_theme_duration(ANIM_F(12));
    if (!duration || !lcd_active())
        return;
    ui_scene_interrupt();
    int handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    if (handle <= 0)
        return;

    fb_data *source = core_get_data_pinned(handle);
    fb_data *framebuffer = lcd_framebuffer_default.fb_ptr;
    memcpy(source, framebuffer, FRAMEBUFFER_SIZE);
    cpu_boost(true);
    lcd_hold_updates(true);
    unsigned trace = ui_perf_begin("shutdown-fade");
    struct ui_tween fade = { .start_clock = ui_clock_start(), .frames = duration,
                            .from = UI_FULL_OPACITY, .to = 0 };
    int composed = -1, last = -1;
    while (lcd_active())
    {
        uint32_t elapsed = (uint32_t)USEC_TIMER - fade.start_clock;
        unsigned frame = ui_clock_frame(elapsed);
        int level = ui_tween_sample(&fade, frame);
        if (level != last)
        {
            if (level != composed)
            {
                uint32_t start = USEC_TIMER;
                ui_blit_dim(framebuffer, source, PAGE_PIXELS, level);
                ui_note_render((uint32_t)USEC_TIMER - start);
                composed = level;
            }
            if (present(framebuffer))
                last = level;
        }
        if (!last || frame > duration + ANIM_F(2))
            break;
        ui_clock_sleep(ui_clock_poll_timeout(elapsed, duration, HZ));
    }
    lcd_hold_updates(false);
    cpu_boost(false);
    core_put_data_pinned(source);
    core_free(handle);
    ui_perf_end(trace);
}

static bool slide_motion(void)
{
    return ui_theme_style(UI_STYLE_PAGE_MOTION) == UI_PAGE_MOTION_SLIDE;
}

/* A slide moves the pages below the status bar; the bar changes in place. */
static int slide_top(void)
{
    return sb_visible_height();
}

static unsigned page_frames(bool push)
{
    return push
        ? ui_theme_motion(UI_MOTION_PUSH, PUSH_ROW_FRAMES, PUSH_ROW_FRAMES)
        : ui_theme_motion(UI_MOTION_POP, POP_FRAMES, POP_FRAMES);
}

/* A pan with slide set is a theme's page push (pan_dir 1) or pop (-1). */
static bool capture_scene(struct ui_scene *scene, const void *from, const void *to,
                          bool push, const char *route, int pan_top,
                          int pan_dir, bool slide)
{
    bool panning = pan_top || slide;
    bool staggered = push && !panning;
    unsigned frames = slide ? page_frames(pan_dir > 0)
                    : pan_top ? ui_theme_duration(PAN_FRAMES)
                    : page_frames(push);
    ui_scene_snap_push();
    if (!frames || !lcd_active() || (owner && owner != scene))
        return false;

    bool active = scene->handle > 0;
    if (!active)
    {
        int handle = core_alloc_free_space(SCENE_BYTES);
        if (handle <= 0)
            return false;
        scene->handle = handle;
        own_transition(scene, route);
    }
    int offset = push ? LCD_WIDTH : 0;
    int end = push ? 0 : LCD_WIDTH;
    bool reverse_pan = active && panning && pan_top == scene->pan_top &&
        slide == scene->slide &&
        to == (scene->target_front ? scene->back_page : scene->front_page) &&
        from == (scene->target_front ? scene->front_page : scene->back_page);
    if (panning)
    {
        offset = pan_dir * LCD_WIDTH;
        if (reverse_pan)
        {
            push = !scene->target_front;
            end = push ? 0 : -offset;
        }
    }
    if (reverse_pan ||
        (active && !staggered && !panning && !scene->pan_top && !scene->slide &&
         (!scene->capturing || push != scene->target_front) &&
         from == (push ? scene->back_page : scene->front_page) &&
         to == (push ? scene->front_page : scene->back_page)))
    {
        offset = scene->last_offset != -1 ? scene->last_offset : scene->tween.from;
        /* The redirected destination is already painted. Start incremental
         * updates from that page's own pixels, including its status bar. */
        fb_data *data = core_get_data_pinned(scene->handle);
        memcpy(lcd_framebuffer_default.fb_ptr, surface(data, push), FRAMEBUFFER_SIZE);
        core_put_data_pinned(data);
    }
    else
    {
        fb_data *data = core_get_data_pinned(scene->handle);
        const fb_data *previous = lcd_framebuffer_default.fb_ptr;
        if (active)
        {
            previous = surface(data, !scene->target_front);
            if (scene->last_offset != -1)
            {
                compose_pages(scene, data + 2 * PAGE_PIXELS, surface(data, false),
                              surface(data, true), scene->last_offset);
                previous = data + 2 * PAGE_PIXELS;
            }
        }
        fb_data *snapshot = surface(data, !push);
        if (snapshot != previous)
            memcpy(snapshot, previous, FRAMEBUFFER_SIZE);
        bool split = menu_pane_take_screen();
        if (push)
            scene->back_split = split;
        else
            scene->front_split = split;
        core_put_data_pinned(data);
        scene->back_page = push ? from : to;
        scene->front_page = push ? to : from;
    }

    scene->target_front = push;
    scene->pan_top = pan_top;
    scene->slide = slide;
    scene->full_screen = scene->back_still = false;
    scene->staggered = staggered;
    scene->reuse_target = reverse_pan;
    scene->row_y = scene->row_count = 0;
    scene->tween = (struct ui_tween){
        .from = offset, .to = end, .frames = frames,
        .ease_in = !push && !slide, .easing = slide ? ui_theme_easing() : 0,
    };
    scene->last_offset = scene->composed_offset = -1;
    scene->capturing = true;
    scene->automatic = false;
    return true;
}

bool ui_scene_capture(struct ui_scene *scene, const void *from, const void *to,
                      bool push)
{
    const char *route = push ? "push" : "pop";
    if (slide_motion())
        return capture_scene(scene, from, to, true, route, slide_top(),
                             push ? 1 : -1, true);
    return capture_scene(scene, from, to, push, route, 0, 0, false);
}

bool ui_scene_capture_pan(struct ui_scene *scene, const void *from, const void *to,
                          int direction, int top)
{
    return capture_scene(scene, from, to, true, "pan", top, direction, false);
}

void ui_scene_boot(struct ui_scene *scene)
{
    if (capture_scene(scene, scene, NULL, true, "boot", 0, 0, false))
        scene->automatic = true;
}

void ui_scene_enter(struct ui_scene *scene, const void *from, const void *to, bool push,
                    bool full_screen)
{
    const void *restored = restored_page;
    restored_page = NULL;
    root_page = to;
    if (restored == to)
        return;
    if (from != to && ui_scene_capture(scene, from, to, push))
    {
        scene->root_entry = true;
        scene->automatic = true;
        /* Full-screen slides move at constant speed; menu slides ease. */
        if (full_screen && scene->slide)
            scene->tween.easing = UI_EASE_LINEAR;
        scene->full_screen = full_screen;
        /* Now Playing slides in over the pane's backdrop, spread across the
         * screen in place of the menu. */
        if (full_screen && push && scene->slide && scene->back_split)
        {
            fb_data *data = core_get_data_pinned(scene->handle);
            menu_pane_backdrop(surface(data, false));
            core_put_data_pinned(data);
            scene->back_still = true;
        }
    }
}

const void *ui_scene_modal_parent(void)
{
    if (owner && owner->root_entry && owner->capturing && owner_thread == thread_self())
        return owner->target_front ? owner->back_page : owner->front_page;
    return root_page;
}

void ui_scene_modal_restored(const void *page)
{
    restored_page = page;
}

static bool capture_panel(struct ui_scene *scene, bool vertical)
{
    bool slide = !vertical && slide_motion();
    unsigned frames = vertical
        ? ui_theme_motion(UI_MOTION_SHEET_IN, MODAL_UP_FRAMES, MODAL_UP_FRAMES)
        : page_frames(true);
    if (!frames || !lcd_active())
        return false;

    const void *parent = vertical ? ui_scene_modal_parent() : NULL;
    if (owner && owner != scene && owner_thread == thread_self() &&
        owner->backdrop_handle > 0 && owner->target_front)
        settle_panel(owner);
    else
        ui_scene_interrupt();
    if (owner)
        return false;
    scene->backdrop_handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    if (scene->backdrop_handle <= 0)
        return false;

    fb_data *backdrop = core_get_data_pinned(scene->backdrop_handle);
    memcpy(backdrop, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    core_put_data_pinned(backdrop);
    scene->vertical = vertical;
    /* A panel page often hides the status bar, so the whole screen slides. */
    scene->pan_top = 0;
    scene->slide = slide;
    scene->staggered = !vertical && !slide;
    scene->row_y = scene->row_count = 0;
    scene->back_page = parent;
    scene->status_height = vertical ? sb_visible_height() : 0;
    scene->dim_color = vertical ? ui_theme_rgb565(UI_COLOR_DIM) : 0;
    scene->target_front = true;
    scene->last_offset = scene->composed_offset = -1;
    scene->tween = (struct ui_tween){
        .from = vertical ? LCD_HEIGHT : LCD_WIDTH, .to = 0, .frames = frames,
        .easing = slide ? UI_EASE_LINEAR : 0,
    };
    return true;
}

void ui_scene_show_modal(struct ui_scene *scene, const char *route)
{
    if (scene->backdrop_handle <= 0)
        return;
    if (owner || !lcd_active())
    {
        release_scene(scene);
        return;
    }
    scene->handle = core_alloc_free_space(PANEL_WORKSPACE_BYTES);
    if (scene->handle <= 0)
    {
        release_scene(scene);
        return;
    }
    scene->capturing = true;
    scene->automatic = true;
    own_transition(scene, route);
}

/* With five fractional bits, the RGB565 lanes can share one unsigned
 * multiply. The mask drops the padding and any borrow between lanes. */
static inline fb_data fade_pixel(fb_data front, fb_data back, unsigned weight)
{
    const uint32_t mask = 0x07e0f81f;
    uint32_t f = (front | ((uint32_t)front << 16)) & mask;
    uint32_t b = (back | ((uint32_t)back << 16)) & mask;
    uint32_t c = (b + (((f - b) * weight) >> 5)) & mask;
    return c | (c >> 16);
}

static void compose_fade(fb_data *out, const fb_data *front, const fb_data *back,
                         unsigned opacity, const int16_t *art_top,
                         const int16_t *art_end)
{
    unsigned weight = (opacity + 4) >> 3;
    /* A tile spans one framebuffer cache line. Only the silhouette rows
     * test individual columns. The solid interior copies directly. */
    for (int x = 0; x < LCD_WIDTH; x += 16)
    {
        int n = MIN(16, LCD_WIDTH - x);
        int top = LCD_HEIGHT, end = 0, solid_top = 0, solid_end = LCD_HEIGHT;
        if (art_top)
            for (int k = 0; k < n; k++)
            {
                top = MIN(top, art_top[x + k]);
                end = MAX(end, art_end[x + k]);
                solid_top = MAX(solid_top, art_top[x + k]);
                solid_end = MIN(solid_end, art_end[x + k]);
            }
        for (int y = 0, i = x; y < LCD_HEIGHT; y++, i += LCD_WIDTH)
        {
            if (y < top || y >= end)
            {
                if (!weight)
                    memcpy(out + i, back + i, n * sizeof(*out));
                else
                    for (int k = 0; k < n; k++)
                        out[i + k] = fade_pixel(front[i + k], back[i + k], weight);
            }
            else if (y >= solid_top && y < solid_end)
                memcpy(out + i, front + i, n * sizeof(*out));
            else
                for (int k = 0; k < n; k++)
                    out[i + k] = y >= art_top[x + k] && y < art_end[x + k]
                               ? front[i + k] : fade_pixel(front[i + k], back[i + k], weight);
        }
    }
}

bool ui_scene_present_fade(struct ui_scene *scene, unsigned opacity,
                           const int16_t *art_top, const int16_t *art_end)
{
    if (scene->backdrop_handle <= 0 || owner || !lcd_active())
        return false;
    const fb_data *front = lcd_framebuffer_default.fb_ptr;
    if (opacity >= UI_FULL_OPACITY)
    {
        if (scene->handle > 0)
            scene->handle = core_free(scene->handle);
        return present(front);
    }
    if (scene->handle <= 0)
        scene->handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    if (scene->handle <= 0)
        return false;
    fb_data *out = core_get_data_pinned(scene->handle);
    const fb_data *back = core_get_data_pinned(scene->backdrop_handle);
    compose_fade(out, front, back, opacity, art_top, art_end);
    bool shown = present(out);
    core_put_data_pinned((void *)back);
    core_put_data_pinned(out);
    return shown;
}

bool ui_scene_restore_modal(struct ui_scene *scene)
{
    if (scene->backdrop_handle <= 0 || (owner && owner != scene))
    {
        release_scene(scene);
        return false;
    }
    fb_data *back = core_get_data_pinned(scene->backdrop_handle);
    memcpy(lcd_framebuffer_default.fb_ptr, back, FRAMEBUFFER_SIZE);
    core_put_data_pinned(back);
    release_scene(scene);
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    ui_scene_modal_restored(scene->back_page);
    return true;
}

void ui_scene_set_parent_painter(bool (*paint)(const void *page))
{
    parent_painter = paint;
}

static void swap_backdrop(struct ui_scene *scene)
{
    uint32_t *back = core_get_data_pinned(scene->backdrop_handle);
    uint32_t *fb = (uint32_t *)lcd_framebuffer_default.fb_ptr;
    for (size_t i = 0; i < FRAMEBUFFER_SIZE / sizeof(*fb); i++)
    {
        uint32_t pixels = fb[i];
        fb[i] = back[i];
        back[i] = pixels;
    }
    core_put_data_pinned(back);
}

bool ui_scene_repaint_backdrop(struct ui_scene *scene)
{
    if (scene->backdrop_handle <= 0 || !parent_painter || owner)
        return false;
    /* The painter draws into the framebuffer, so the page on screen waits in
     * the backdrop meanwhile and nothing reaches the LCD. */
    swap_backdrop(scene);
    bool held = lcd_updates_held();
    lcd_hold_updates(true);
    bool painted = parent_painter(scene->back_page);
    lcd_hold_updates(held);
    swap_backdrop(scene);
    FOR_NB_SCREENS(i)
        ui_take_dirty(i);
    return painted;
}

bool ui_scene_capture_modal(struct ui_scene *scene)
{
    return capture_panel(scene, true);
}

void ui_scene_open_modal(struct ui_scene *scene, const char *route)
{
    if (ui_scene_capture_modal(scene))
        ui_scene_show_modal(scene, route);
}

void ui_scene_open_page(struct ui_scene *scene, const char *route)
{
    if (capture_panel(scene, false))
        ui_scene_show_modal(scene, route);
}

bool ui_scene_close_modal(struct ui_scene *scene, bool animate, const char *route)
{
    if (scene->backdrop_handle <= 0)
        return false;
    unsigned frames = !animate ? 0 : scene->vertical
        ? ui_theme_motion(UI_MOTION_SHEET_OUT, MODAL_DOWN_FRAMES,
                          MODAL_DOWN_FRAMES)
        : page_frames(false);
    if (!frames || !lcd_active() || (owner && owner != scene))
    {
        release_scene(scene);
        return false;
    }
    if (owner == scene && scene->staggered)
        ui_scene_snap_push();
    bool was_opening = owner == scene;
    int offset = was_opening
        ? (scene->last_offset >= 0 ? scene->last_offset : scene->tween.from) : 0;
    if (scene->handle <= 0)
        scene->handle = core_alloc_free_space(PANEL_WORKSPACE_BYTES);
    if (scene->handle <= 0)
    {
        release_scene(scene);
        return false;
    }
    complete_transition(scene);
    own_transition(scene, route);

    fb_data *front = core_get_data_pinned(scene->handle);
    fb_data *backdrop = core_get_data_pinned(scene->backdrop_handle);
    memcpy(front, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    memcpy(lcd_framebuffer_default.fb_ptr, backdrop, FRAMEBUFFER_SIZE);
    core_put_data_pinned(backdrop);
    core_put_data_pinned(front);
    scene->target_front = false;
    scene->last_offset = scene->composed_offset = -1;
    scene->tween = (struct ui_tween){
        .start_clock = ui_clock_start(), .from = offset,
        .to = scene->vertical ? LCD_HEIGHT : LCD_WIDTH, .frames = frames,
        .ease_in = !scene->slide,
        .easing = scene->slide ? UI_EASE_LINEAR : 0,
    };
    while (owner == scene)
    {
        ui_scene_update(scene, false);
        if (owner != scene)
            break;
        int timeout = ui_scene_timeout(scene, TIMEOUT_BLOCK);
#if defined(HAVE_USB_PODLINK) || (defined(SIMULATOR) && defined(IPOD_6G))
        if (button_queue_wait_pending(timeout))
            break;
#else
        ui_clock_sleep(timeout);
#endif
    }
    if (owner == scene)
        ui_scene_cancel(scene);
    if (scene->vertical)
        ui_scene_modal_restored(scene->back_page);
    return true;
}

bool ui_scene_automatic(void)
{
    return owner && owner->automatic && owner_thread == thread_self();
}

int ui_scene_poll(int timeout)
{
    /* A modal that returns inside its parent resumes this input pump. Its
     * restored pixels have no bearing on a later root navigation. */
    restored_page = NULL;
    if (rock_ui_raw() || !ui_scene_automatic())
        return timeout;
    struct ui_scene *scene = owner;
    if (scene->capturing)
        ui_scene_start(scene);
    else
        ui_scene_update(scene, false);
    return ui_scene_timeout(scene, timeout);
}

void ui_scene_start(struct ui_scene *scene)
{
    if (!scene->capturing)
        return;
    if (scene->staggered)
    {
        scene->push_top = viewportmanager_theme_enabled(SCREEN_MAIN)
                        ? ui_theme_metric(UI_METRIC_STATUS_HEIGHT) : 0;
        scene->dim_color = ui_theme_rgb565(UI_COLOR_BACKGROUND);
        set_push_duration(scene);
    }
    scene->capturing = false;
    scene->tween.start_clock = ui_clock_start();
    /* The first frame of a reversed pan is the pose already on screen. */
    ui_scene_update(scene, !scene->reuse_target);
    scene->reuse_target = false;
}

void ui_scene_update(struct ui_scene *scene, bool target_changed)
{
    if (owner != scene || scene->capturing)
        return;
    if (!lcd_active())
    {
        release_scene(scene);
        return;
    }

    target_changed |= scene->rows_changed;
    scene->rows_changed = false;
    unsigned frame = ui_clock_frame((uint32_t)USEC_TIMER - scene->tween.start_clock);
    if (frame >= scene->tween.frames || scene->tween.from == scene->tween.to)
    {
        if (present(lcd_framebuffer_default.fb_ptr))
        {
            if (scene->backdrop_handle > 0 && scene->target_front)
                settle_panel(scene);
            else
                release_scene(scene);
        }
        return;
    }

    int offset = position(scene, frame);
    int pose = scene->staggered ? (int)frame : offset;
    if (pose == scene->last_offset && !target_changed)
        return;

    fb_data *data = core_get_data_pinned(scene->handle);
    bool panel = scene->backdrop_handle > 0;
    fb_data *out = data + (panel ? PAGE_PIXELS : 2 * PAGE_PIXELS);
    if (target_changed || scene->composed_offset != pose)
    {
        uint32_t start = USEC_TIMER;
        fb_data *front = panel ? data : surface(data, true);
        fb_data *back = panel ? core_get_data_pinned(scene->backdrop_handle)
                              : surface(data, false);
        if (target_changed && (!panel || scene->target_front))
            memcpy(scene->target_front ? front : back, lcd_framebuffer_default.fb_ptr,
                   FRAMEBUFFER_SIZE);
        if (target_changed && !panel)
        {
            bool split = menu_pane_split() != 0;
            if (scene->target_front)
                scene->front_split = split;
            else
                scene->back_split = split;
            /* A full page passes a menu at constant speed. */
            if (scene->slide && !scene->full_screen)
                scene->tween.easing = scene->back_split != scene->front_split
                                      ? UI_EASE_LINEAR : ui_theme_easing();
        }
        if (scene->vertical)
            compose_modal(out, back, front, offset,
                          scene->target_front ? 0 : scene->status_height,
                          scene->dim_color);
        else if (scene->staggered)
            compose_push(scene, out, back, front, frame, offset);
        else
            compose_pages(scene, out, back, front, offset);
        if (panel)
            core_put_data_pinned(back);
        scene->composed_offset = pose;
        ui_note_render((uint32_t)USEC_TIMER - start);
    }
    bool submitted = present(out);
    core_put_data_pinned(data);
    if (submitted)
        scene->last_offset = pose;
    else
        yield();
}

int ui_scene_timeout(const struct ui_scene *scene, int timeout)
{
    if (owner != scene || scene->capturing || timeout == TIMEOUT_NOBLOCK)
        return timeout;

    uint32_t elapsed = (uint32_t)USEC_TIMER - scene->tween.start_clock;
    unsigned frame = ui_clock_frame(elapsed);
    if (frame >= scene->tween.frames || scene->tween.from == scene->tween.to)
        return 1;
    /* Retry a composed frame the LCD refused. A frame the clock has reached
     * waits for its presentation slot, so a half-rate scene keeps its
     * parity. */
    if (scene->composed_offset != scene->last_offset)
        return TIMEOUT_NOBLOCK;

    int ticks = ui_clock_poll_timeout(elapsed, scene->tween.frames, HZ);
    return timeout == TIMEOUT_BLOCK ? ticks : MIN(ticks, timeout);
}
