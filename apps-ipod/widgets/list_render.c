/* Native retained list renderer. Adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "action.h"
#include "button.h"
#include "core_alloc.h"
#include <ctype.h>
#include "dir.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "rbunicode.h"
#include "scroll_engine.h"
#include "settings/settings.h"
#include "strcasecmp.h"
#include "system.h"
#include "audio.h"
#include "metadata.h"
#include "draw/bmp.h"
#include "draw/color.h"
#include "draw/line.h"
#include "draw/round_rect.h"
#include "draw/scrollbar.h"
#include "draw/text_wrap.h"
#include "draw/viewport.h"
#include "metadata/album_palette.h"
#include "metadata/art_cache.h"
#include "system/activity.h"
#include "ui/frame.h"
#include "ui/menu_pane.h"
#include "ui/native_chrome.h"
#include "ui/native_theme_runtime.h"
#include "ui/pace.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "ui/statusbar.h"
#include "ui/theme_art.h"
#include "plugins/rock_compat.h"
#include "list_render.h"
#include "menu.h"
#include "ui/scene.h"
#include "list_hwscroll.h"
#include "list_select.h"
#include "list_quickscroll.h"

struct list_geometry {
    struct ui_rect background, title, header, rows, notes;
    int pitch;
};

enum {
    GLIDE_F = ANIM_F(8),
    SPRING_F = ANIM_F(10),
    ROW_UNIT = 256,
    HALF_ROW = ROW_UNIT / 2,
    MAX_WHEEL_LAG = 3 * ROW_UNIT,
    /* Stock's overlay tile, and its letter's baseline on it. */
    INDEX_TILE_WIDTH = 74,
    INDEX_TILE_HEIGHT = 70,
    INDEX_TEXT_BASELINE = 51,
    INDEX_PRELOAD_ROWS = 50,
    INDEX_GLYPHS = 40,
    INDEX_NEAR = 4,
    INDEX_PREFETCH_ROWS = 12,
    INDEX_SCRIM_RETAIN = 56,  /* 78% dim */
    INDEX_BORDER_RETAIN = 225, /* 12% white */
};

struct list_pose {
    int64_t top, cursor;
};

/* A glide samples frame n at refresh n after it starts. Under snapped
 * scrolling the marker instead steps one frame per refresh it reaches:
 * painted counts the frames drawn. */
struct list_motion {
    struct list_pose from, live, rest;
    uint32_t started;
    unsigned painted, trace;
    unsigned glide_frames, spring_frames;
    int visible, count, span;
    struct ui_rect bounds;
    int pitch;
    bool wheel, snap;
};

/* A theme gradient as RGB565 rows, for a fill `height` rows tall. */
struct gradient_rows {
    uint16_t color[LCD_HEIGHT];
    int height;
};

/* The wheel's letter jump for one list, and its overlay. */
struct list_index {
    struct quickscroll qs;
    const struct gui_synclist *list;
    unsigned generation;
    char key[QUICKSCROLL_KEY_SIZE];
    bool visible;
};

/* What the last paint left on screen, to tell a frame in which only the
 * selection marker moved, or one the panel can scroll to. on_panel: the
 * paint reached the LCD, after which ui_hwscroll_writes() read writes.
 * held: a page transition held the paint back, when the panel had shown
 * the framebuffer whole `framebuffers` times. complete: no text outside
 * the rows waited for its glyphs. skipped: for each of the `slots` rows
 * painted from `top`, which of its texts, in the order drawn, waited for
 * glyphs; a later font epoch repaints and resends only the rows this
 * marks. */
#define SHOWN_ROWS 24
struct list_shown {
    struct ui_rect rows, marker;
    int pitch, top, top_px, cursor_y, cursor_height, lit, selected, indicator;
    unsigned theme, palette, art, font, artist, writes, framebuffers;
    int art_handle;
    bool valid, on_panel, held, complete;
    int slots;
    unsigned char skipped[SHOWN_ROWS];
};

struct native_list {
    struct ui_theme theme;
    unsigned int colors[UI_COLOR_COUNT];
    unsigned theme_generation, palette_generation;
    bool colors_current;
    int text_font, title_font, icon_font, value_font;
    struct ui_view root, title, header, rows, notes, art, index_view, pane;
    struct ui_page page;
    struct ui_layer backdrop;
    unsigned art_generation, artist_generation;
    unsigned int art_text_color;
    int art_handle;
    int art_mode;
    struct gui_synclist *owner;
    unsigned generation;
    int selection;
    unsigned font_epoch;
    int prefetched_top;
    struct list_motion motion;
    struct list_shown shown;
    /* shown.slots and shown.skipped for the paint in progress. */
    int slots;
    unsigned char skipping[SHOWN_ROWS];
    struct list_index overlay;
    int index_faces[3];
    char index_shown[QUICKSCROLL_KEY_SIZE];
    /* Row and notes painters run serially while the renderer is pinned. */
    union {
        struct {
            char name[MAX_PATH], value[96], playing[MAX_PATH], home[MAX_PATH];
        } row;
        char notes[2 * MAX_PATH];
    } scratch;
    int row_shift, cursor_y, cursor_height;
    /* Columns the scroll indicator takes from the rows' right edge. */
    int row_inset;
    /* The classic indicator's strength in the last paint of the rows. */
    int indicator;
    struct gradient_rows bar_rows, thumb_rows;
    uint32_t marquee_started, marquee_frame;
    /* The refresh at which the last snapped or hardware-scroll frame
     * reaches the panel; batched while a glide follows the latter. */
    uint32_t presented;
    bool has_marquee, snap_frame, batched;
    /* The last frame left rows of waiting text for the next one. */
    bool repairing;
    int child_next;
    unsigned child_glyphs;
    uint32_t child_after;
    /* While every row paints for a hardware-scroll frame: how many rows of
     * waiting text may still change; the rest keep their text as shown. */
    bool holding;
    int repair_rows;
    unsigned char mark_mask[2];
};

static int renderer_handle;
static struct ui_pace glide_pace;
static unsigned child_token;

#define CHILD_IDLE_US 120000
#define CHILD_GLYPHS 96
#define CHILD_PAGES 64

bool list_render_child_current(unsigned token)
{
    return token == __atomic_load_n(&child_token, __ATOMIC_RELAXED);
}

static void child_reset(struct native_list *render)
{
    /* The list thread is the sole writer. The directory worker polls it. */
    __atomic_store_n(&child_token, child_token + 1, __ATOMIC_RELAXED);
    if (render->child_glyphs)
        font_prefetch_speculative_cancel();
    render->child_after = USEC_TIMER + CHILD_IDLE_US;
    render->child_next = 0;
    render->child_glyphs = 0;
}

/* The cover backdrop of the last list drawn. */
static int backdrop_handle(void)
{
    if (renderer_handle <= 0)
        return 0;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    int handle = render->backdrop.handle;
    core_put_data_pinned(render);
    return MAX(0, handle);
}

void list_render_init(void)
{
    if (renderer_handle > 0)
        return;
    renderer_handle = core_alloc_free_space(sizeof(struct native_list));
    if (renderer_handle > 0)
    {
        struct native_list *render = core_get_data_pinned(renderer_handle);
        memset(render, 0, sizeof(*render));
        for (unsigned i = 0; i < ARRAYLEN(render->index_faces); i++)
            render->index_faces[i] = -1;
        render->text_font = render->title_font = render->icon_font = -1;
        render->value_font = -1;
        render->theme_generation = (unsigned)-1;
        core_put_data_pinned(render);
        ui_scene_set_backdrop_source(backdrop_handle);
    }
}

static void refresh_tokens(struct native_list *render)
{
    unsigned generation = ui_theme_generation();
    if (render->theme_generation == generation)
        return;

    ui_theme_snapshot(&render->theme);
    render->theme_generation = generation;
    render->colors_current = false;
    render->bar_rows.height = render->thumb_rows.height = 0;
    font_unload(render->text_font);
    font_unload(render->title_font);
    font_unload(render->icon_font);
    font_unload(render->value_font);
    int glyphs = global_settings.glyphs_to_cache;
    render->text_font = ui_theme_font_load(&render->theme, UI_FONT_LIST, glyphs);
    render->title_font = ui_theme_font_load(&render->theme, UI_FONT_TITLE, glyphs);
    render->icon_font = ui_theme_font_load(&render->theme, UI_FONT_ICON, glyphs);
    /* Only rows that show their value in words draw it. */
    render->value_font = render->theme.style[UI_STYLE_ACCESSORY] ==
                         UI_ACCESSORY_SELECTED ?
        ui_theme_font_load(&render->theme, UI_FONT_LIST_VALUE, glyphs) : -1;
    round_rect_build_mask(render->mark_mask, 1,
                          render->theme.metric[UI_METRIC_LIST_SELECTION_ALPHA]);
}

static int row_pitch(const struct gui_synclist *list, enum screen_type screen,
                     const uint16_t *metric)
{
    if (list->default_viewport && list->callback_get_item_albumart)
        return metric[UI_METRIC_LIST_ART_ROW_HEIGHT];
    if (list->line_height_override > 0)
        return list->line_height_override;
    if (list->default_viewport && viewportmanager_theme_enabled(screen))
        return metric[UI_METRIC_ROW_HEIGHT];
    return MAX(list->line_height[screen], 1);
}

/* Under style.list_rows=fill the rows take the height the last whole row
 * leaves, a pixel each from the top row down: stock's nine rows in 220
 * pixels are four of 25 and five of 24. */
static int row_spare(const struct ui_theme *theme, int height, int pitch)
{
    if (theme->style[UI_STYLE_LIST_ROWS] != UI_LIST_ROWS_FILL || pitch <= 0)
        return 0;
    return height % pitch;
}

/* The top of the row `slot` rows below the first, from the first's top. */
static int slot_y(int slot, int pitch, int spare)
{
    return slot * pitch + MIN(MAX(slot, 0), spare);
}

static struct ui_rect take_top(struct ui_rect *rect, int height)
{
    height = MIN(rect->height, MAX(0, height));
    struct ui_rect top = { rect->x, rect->y, rect->width, height };
    rect->y += height;
    rect->height -= height;
    return top;
}

/* A menu that shares the screen with the pane beside it. */
static bool split_list(const struct gui_synclist *list)
{
    return list->callback_get_item_pane && list->default_viewport &&
           !list->selection_color &&
           viewportmanager_theme_enabled(SCREEN_MAIN) && menu_pane_themed();
}

static struct list_geometry measure(const struct gui_synclist *list,
                                    enum screen_type screen,
                                    const uint16_t *metric)
{
    const struct viewport *parent = list->parent[screen];
    bool chrome = viewportmanager_theme_enabled(screen);
    bool full = chrome && list->default_viewport;
    struct ui_rect rows = { parent->x, parent->y, parent->width,
                            parent->height };
    struct list_geometry g = {
        .background = rows,
        .pitch = row_pitch(list, screen, metric),
    };

    if (full)
    {
        int top = metric[UI_METRIC_STATUS_HEIGHT];
        g.background = (struct ui_rect){0, top, LCD_WIDTH, LCD_HEIGHT - top};
        if (split_list(list))
        {
            g.background.width = MENU_PANE_X;
            rows.width = MIN(rows.width, MENU_PANE_X - rows.x);
        }

        if (list->callback_get_item_albumart && !list->header && !list->show_notes)
        {
            int extra = MIN(rows.y - top, metric[UI_METRIC_LIST_ART_TOP_GAP]);
            rows.y -= MAX(extra, 0);
            rows.height += MAX(extra, 0) + metric[UI_METRIC_LIST_ART_BOTTOM_GAP];
        }
    }
    else if (!chrome && list->title && rows.height / g.pitch > 2)
    {
        g.title = take_top(&rows, g.pitch);
    }

    if (list->header)
    {
        if (full)
            rows.height += metric[UI_METRIC_LIST_HEADER_BOTTOM_GAP];
        g.header = take_top(&rows, MIN(metric[UI_METRIC_LIST_HEADER_HEIGHT],
                                     MAX(0, rows.height - g.pitch)));
    }
    if (list->show_notes && list->callback_get_item_description)
    {
        int height = MIN(metric[UI_METRIC_LIST_NOTES_HEIGHT],
                         MAX(0, rows.height - g.pitch));
        rows.height -= height;
        g.notes = (struct ui_rect){rows.x, rows.y + rows.height, rows.width,
                                   height};
    }
    rows.height = MAX(0, rows.height);
    g.rows = rows;
    return g;
}

int list_render_row_height(const struct gui_synclist *list, enum screen_type screen)
{
    if (renderer_handle <= 0)
        return MAX(1, list->line_height[screen]);
    struct native_list *render = core_get_data_pinned(renderer_handle);
    refresh_tokens(render);
    int height = row_pitch(list, screen, render->theme.metric);
    core_put_data_pinned(render);
    return height;
}

int list_render_visible_rows(const struct gui_synclist *list, enum screen_type screen)
{
    if (renderer_handle <= 0)
        return list->parent[screen]->height / MAX(1, list->line_height[screen]);
    struct native_list *render = core_get_data_pinned(renderer_handle);
    refresh_tokens(render);
    struct list_geometry g = measure(list, screen, render->theme.metric);
    core_put_data_pinned(render);
    return g.rows.height / g.pitch;
}

static bool motion_enabled(void)
{
    return !ui_scrolling_held() &&
           ui_theme_motion(UI_MOTION_LIST, GLIDE_F, GLIDE_F) != 0;
}

static int64_t fixed_row(int index)
{
    return (int64_t)index * ROW_UNIT;
}

static bool motion_owner(const struct native_list *render, const struct gui_synclist *list)
{
    return render->owner == list && render->generation == list->generation;
}

static void motion_stop(struct list_motion *motion)
{
    ui_perf_end(motion->trace);
    motion->trace = 0;
    motion->visible = 0;
}

static unsigned motion_duration(const struct list_motion *motion)
{
    return motion->glide_frames +
           (motion->live.top == motion->rest.top ? 0 : motion->spring_frames);
}

static bool motion_matches(const struct list_motion *motion, const struct list_geometry *g,
                            const struct gui_synclist *list)
{
    return motion->visible > 0 && motion->count == list->nb_items &&
           motion->span == list->selected_size && motion->pitch == g->pitch &&
           !memcmp(&motion->bounds, &g->rows, sizeof(g->rows)) &&
           motion->rest.cursor == fixed_row(list->selected_item) &&
           motion->rest.top == fixed_row(list->start_item[SCREEN_MAIN]);
}

static int64_t ease_out(int64_t from, int64_t to, unsigned frame, unsigned duration)
{
    const struct ui_tween weight = { .to = 1024, .frames = duration };
    return from + (to - from) * ui_tween_sample(&weight, frame) / weight.to;
}

/* Keep the bar in view while viewport and selection catch up independently. */
static int64_t limit_top(const struct list_motion *motion, struct list_pose pose)
{
    int64_t back = fixed_row(motion->visible - motion->span) + HALF_ROW;
    int64_t top = MAX(pose.top, pose.cursor - back);
    return MIN(top, pose.cursor + HALF_ROW);
}

/* Under snapped scrolling the eased tail would repeat a pixel. A marker
 * with at least a pixel per frame left to go keeps moving a pixel or more
 * on every frame until it arrives. */
static int64_t marker_tail(const struct list_motion *motion, int64_t cursor,
                           unsigned frame)
{
    int pitch = MAX(1, motion->pitch);
    int64_t pixel = (ROW_UNIT + pitch - 1) / pitch;
    int64_t from = motion->from.cursor, to = motion->live.cursor;
    int64_t left = (int64_t)(motion->glide_frames - frame) * pixel;
    int64_t span = (int64_t)motion->glide_frames * pixel;
    if (to - from >= span)
        return MIN(cursor, to - left);
    if (from - to >= span)
        return MAX(cursor, to + left);
    return cursor;
}

static struct list_pose motion_sample(const struct list_motion *motion, unsigned frame)
{
    struct list_pose pose = motion->live;
    unsigned glide = motion->glide_frames;
    if (frame < glide)
    {
        pose.top = ease_out(motion->from.top, pose.top, frame, glide);
        pose.cursor = ease_out(motion->from.cursor, pose.cursor, frame, glide);
        if (motion->snap)
            pose.cursor = marker_tail(motion, pose.cursor, frame);
    }
    else if (pose.top != motion->rest.top)
    {
        pose.top = ease_out(pose.top, motion->rest.top, frame - glide, motion->spring_frames);
    }
    pose.top = limit_top(motion, pose);
    return pose;
}

void list_render_settle(const struct gui_synclist *list)
{
    if (renderer_handle <= 0)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    if (motion_owner(render, list))
        motion_stop(&render->motion);
    core_put_data_pinned(render);
}

void list_render_retarget(const struct gui_synclist *list, int selection, int top,
                          int offset, bool wheel)
{
    unsigned glide = ui_theme_motion(UI_MOTION_LIST, GLIDE_F, GLIDE_F);
    if (!glide || ui_scrolling_held() || renderer_handle <= 0 || !list->nb_items ||
        !list->parent[SCREEN_MAIN])
        return;

    struct native_list *render = core_get_data_pinned(renderer_handle);
    if (!motion_owner(render, list))
        goto done;

    refresh_tokens(render);
    struct list_geometry g = measure(list, SCREEN_MAIN, render->theme.metric);
    int visible = g.rows.height / g.pitch;
    int span = list->selected_size;
    struct list_motion *motion = &render->motion;
    if (visible < span || g.rows.width <= 0)
    {
        motion_stop(motion);
        goto done;
    }

    uint32_t now = USEC_TIMER;
    uint32_t elapsed = now - motion->started;
    unsigned frame = ui_clock_frame(elapsed);
    struct list_pose from = { fixed_row(top), fixed_row(selection) };
    bool continuing = motion->visible == visible && motion->pitch == g.pitch &&
                      motion->count == list->nb_items && motion->span == span &&
                      !memcmp(&motion->bounds, &g.rows, sizeof(g.rows)) &&
                      motion->rest.top == from.top && motion->rest.cursor == from.cursor;
    bool snap =
        render->theme.style[UI_STYLE_LIST_SCROLL] == UI_LIST_SCROLL_SNAP;
    if (continuing)
    {
        from = motion_sample(motion, snap ? motion->painted : frame);
        /* Carry the frame phase through rapid wheel events. Restarting the
         * clock at each detent would stall a glide when detents arrive < 1 f apart. */
        now -= elapsed - ui_frame_clock(frame);
    }

    struct list_pose rest = { fixed_row(list->start_item[SCREEN_MAIN]),
                              fixed_row(list->selected_item) };
    struct list_pose live = rest;
    int last = list->nb_items - span;
    int64_t max_top = fixed_row(MAX(0, list->nb_items - visible));
    int64_t position = (int64_t)selection + (int64_t)offset * span;
    bool wrapped = (offset < 0 && rest.cursor > fixed_row(selection)) ||
                   (offset > 0 && rest.cursor < fixed_row(selection));
    bool physical_end = wheel && !wrapped && (position < 0 || position > last);
    bool fast = wheel && (offset < -1 || offset > 1 ||
                         (continuing && motion->wheel && frame < motion->glide_frames));

    if (snap)
    {
        /* The rows jump to their new place and the marker sets out from
         * where it was on screen. */
        from.cursor += rest.top - from.top;
        from.top = rest.top;
    }
    else if (physical_end)
    {
        bool bottom = offset > 0;
        int64_t edge = bottom ? max_top : 0;
        int64_t prior = continuing ? motion->live.top : from.top;
        int64_t pull = MAX(0, bottom ? prior - edge : edge - prior);
        /* Each extra detent adds half of the remaining travel. */
        pull += (HALF_ROW - pull + 1) / 2;
        live.top = edge + (bottom ? pull : -pull);
    }
    else if (fast && !wrapped && visible >= span + 2 && max_top &&
             rest.cursor != fixed_row(selection))
    {
        int64_t last_row = fixed_row(visible - span);
        int64_t row = rest.cursor - rest.top;
        if (offset > 0 && row >= last_row - ROW_UNIT)
            live.top = MAX(0, rest.cursor - last_row - HALF_ROW);
        else if (offset < 0 && row <= ROW_UNIT)
            live.top = MIN(max_top, rest.cursor + HALF_ROW);
    }

    if (wheel)
    {
        from.cursor = MIN(MAX(from.cursor, rest.cursor - MAX_WHEEL_LAG),
                          rest.cursor + MAX_WHEEL_LAG);
    }
    if (from.top == live.top && from.cursor == live.cursor && live.top == rest.top)
    {
        motion_stop(motion);
        goto done;
    }

    /* A snapped jump costs a whole-list frame; the marker's own frames
     * that follow are cheap enough for every refresh again. */
    glide_pace.recover_full_rate = snap;
    unsigned trace = motion->trace;
    if (!trace)
        trace = ui_perf_begin("list-glide");
    *motion = (struct list_motion) {
        .from = from, .live = live, .rest = rest,
        .started = continuing ? now : ui_clock_start(),
        .painted = 0, .trace = trace,
        .glide_frames = glide,
        .spring_frames = ui_theme_motion(UI_MOTION_LIST, SPRING_F, GLIDE_F),
        .visible = visible, .count = list->nb_items, .span = span,
        .bounds = g.rows, .pitch = g.pitch, .wheel = wheel, .snap = snap,
    };

done:
    core_put_data_pinned(render);
}

static struct ui_rect intersection(struct ui_rect a, struct ui_rect b)
{
    int left = MAX(a.x, b.x), top = MAX(a.y, b.y);
    return (struct ui_rect) {
        left, top,
        MIN(a.x + a.width, b.x + b.width) - left,
        MIN(a.y + a.height, b.y + b.height) - top,
    };
}

static struct ui_rect bounding(struct ui_rect a, struct ui_rect b)
{
    int left = MIN(a.x, b.x), top = MIN(a.y, b.y);
    return (struct ui_rect) {
        left, top,
        MAX(a.x + a.width, b.x + b.width) - left,
        MAX(a.y + a.height, b.y + b.height) - top,
    };
}

static struct ui_rect row_cell(struct ui_rect row, int x, int y, int width,
                               int height, bool rtl)
{
    return (struct ui_rect){
        row.x + (rtl ? row.width - x - width : x), row.y + y, width, height,
    };
}

static bool set_clip(struct viewport *vp, struct ui_rect clip,
                     struct ui_rect cell, int font, unsigned int fg)
{
    struct ui_rect r = intersection(clip, cell);
    if (r.width <= 0 || r.height <= 0)
        return false;
    *vp = *lcd_current_viewport;
    vp->x = r.x;
    vp->y = r.y;
    vp->width = r.width;
    vp->height = r.height;
    vp->flags = VP_DEFAULT_FLAGS;
    vp->buffer = &lcd_framebuffer_default;
    vp->font = font >= 0 ? font : FONT_UI;
    vp->fg_pattern = fg;
    vp->drawmode = DRMODE_FG;
    return true;
}

static void refresh_colors(struct native_list *render)
{
    unsigned generation = album_palette_generation();
    if (render->colors_current && render->palette_generation == generation)
        return;

    struct ui_album_palette palette;
    album_palette_snapshot(&palette);
    static const enum ui_color_role list_colors[] = {
        UI_COLOR_BACKGROUND, UI_COLOR_FOREGROUND, UI_COLOR_MUTED, UI_COLOR_SELECTION,
        UI_COLOR_SELECTED_TEXT, UI_COLOR_LIST_TINT, UI_COLOR_DIM,
        UI_COLOR_ACCENT,
    };
    for (unsigned i = 0; i < ARRAYLEN(list_colors); ++i)
    {
        enum ui_color_role role = list_colors[i];
        uint32_t rgb = ui_theme_color(&render->theme, role, &palette);
        render->colors[role] = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
    }
    render->colors[UI_COLOR_DIM] = color_tone_bound(render->colors[UI_COLOR_DIM], 8, -1);
    render->palette_generation = generation;
    render->colors_current = true;
}

static void fill(struct ui_rect clip, struct ui_rect cell, unsigned int fg)
{
    struct viewport vp;
    if (!set_clip(&vp, clip, cell, FONT_UI, fg))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_viewport(prior);
}

static const uint16_t *gradient_rows(struct native_list *render,
                                     struct gradient_rows *rows,
                                     enum ui_gradient_role role, int height)
{
    height = MIN(MAX(height, 1), LCD_HEIGHT);
    if (rows->height != height)
    {
        const uint32_t *art = role == UI_GRADIENT_SELECTION &&
                              ui_theme_art(&render->theme) ?
                              ui_art_selection(height) : NULL;
        for (int y = 0; y < height; ++y)
        {
            uint32_t rgb = art ? art[y] :
                           ui_theme_gradient_at(&render->theme.gradient[role],
                                                y, height, 0);
            rows->color[y] = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255,
                                         rgb & 255);
        }
        rows->height = height;
    }
    return rows->color;
}

/* Fill `cell` with one colour per row from `colors`, indexed from its top. */
static void fill_rows(struct ui_rect clip, struct ui_rect cell,
                      const uint16_t *colors)
{
    struct viewport vp;
    if (!set_clip(&vp, clip, cell, FONT_UI, 0))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    const uint16_t *color = colors + (vp.y - cell.y);
    for (int y = 0; y < vp.height; ++y)
    {
        lcd_set_foreground(color[y]);
        lcd_fillrect(0, y, vp.width, 1);
    }
    lcd_set_viewport(prior);
}

static unsigned marquee_frames(void)
{
    uint32_t step = lcd_scroll_info.ticks * (1000000 / HZ);
    return MAX(1, ui_clock_frame(step + ui_frame_period() / 2));
}

static unsigned marquee_shift(struct native_list *render, unsigned max_offset)
{
    if (!global_settings.scrolling_enabled || ui_scrolling_held() || !max_offset)
        return 0;
    render->has_marquee = true;
    unsigned elapsed = ui_clock_frame(USEC_TIMER - render->marquee_started);
    /* The stored delay setting is expressed in milliseconds. */
    unsigned delay = ui_clock_frame(global_settings.scroll_delay * 1000);
    unsigned step = MAX(1, global_settings.scroll_step);
    unsigned frames = marquee_frames();
    unsigned trip = (max_offset + step - 1) / step * frames;
    unsigned phase = elapsed % (2 * (delay + trip));
    if (phase < delay)
        return 0;
    phase -= delay;
    if (phase < trip)
        return MIN(max_offset, phase / frames * step);
    phase -= trip;
    if (phase < delay)
        return max_offset;
    return (max_offset - MIN(max_offset, (phase - delay) / frames * step));
}

/* Stock's marquee, kept by the themes with its selection bar: the label
 * waits STOCK_MARQUEE_WAIT ticks of 34.1 ms at its start, then runs left 2
 * columns a tick with a copy STOCK_MARQUEE_GAP columns behind it, and rests
 * one tick less each time the copy reaches the start. The scroll settings
 * do not apply to it. */
#define STOCK_MARQUEE_TICK 34100 /* us */
#define STOCK_MARQUEE_WAIT 38 /* ticks */
#define STOCK_MARQUEE_GAP 26

/* Under style.scrollbar=classic the indicator shows only around a move, as
 * stock's: at full strength for INDICATOR_HOLD after the selection moves or
 * the list opens, then a step weaker each INDICATOR_STEP_US until gone. */
#define INDICATOR_HOLD 270000 /* us */
#define INDICATOR_STEP_US STOCK_MARQUEE_TICK
#define INDICATOR_STEPS 14

static bool classic_indicator(const struct native_list *render)
{
    return render->theme.style[UI_STYLE_SCROLLBAR] == UI_SCROLLBAR_CLASSIC;
}

/* The indicator's strength now, INDICATOR_STEPS (full) down to 0 (gone). */
static int indicator_level(const struct native_list *render)
{
    uint32_t elapsed = USEC_TIMER - render->marquee_started;
    if (elapsed < INDICATOR_HOLD)
        return INDICATOR_STEPS;
    uint32_t steps = (elapsed - INDICATOR_HOLD) / INDICATOR_STEP_US;
    return steps >= INDICATOR_STEPS ? 0 : INDICATOR_STEPS - (int)steps;
}

static bool stock_marquee(const struct native_list *render)
{
    return render->theme.style[UI_STYLE_SELECTION] == UI_SELECTION_BAR;
}

/* The offset into a run of `period` columns, the label's and its gap's. */
static unsigned stock_marquee_shift(struct native_list *render,
                                    unsigned period)
{
    if (!global_settings.scrolling_enabled || ui_scrolling_held())
        return 0;
    render->has_marquee = true;
    unsigned tick = (USEC_TIMER - render->marquee_started) /
                    STOCK_MARQUEE_TICK;
    if (tick < STOCK_MARQUEE_WAIT)
        return 0;
    /* A run's last tick lands back on the start and counts as rest. */
    unsigned run = (period + 1) / 2;
    unsigned phase = (tick - STOCK_MARQUEE_WAIT) %
                     (run + STOCK_MARQUEE_WAIT - 2);
    if (phase >= run)
        return 0;
    return MIN(period, 2 * (phase + 1)) % period;
}

/* Set by any text a paint leaves out until its glyphs load. row_skipped
 * marks such texts by their order in the row, row_texts counting them;
 * row_hold marks the texts to leave out again, as the last paint did. */
static bool text_deferred;
static unsigned char row_skipped, row_hold;
static int row_texts;

/* Rows of changed text one frame sends besides what moves. */
#define REPAIR_ROWS (LIST_HWSCROLL_ROWS / 2)

static bool list_text_ready(int font, const char *text)
{
    bool ready = !(row_hold & 1u << MIN(row_texts, 7)) &&
                 (!font_prefetch_active() || font_prefetch_text(font, text));
    text_deferred |= !ready;
    if (!ready)
        row_skipped |= 1u << MIN(row_texts, 7);
    ++row_texts;
    return ready;
}

/* Texts the clip leaves out count too, so every paint of a row asks for
 * the same texts in the same order. */
static void draw_text(struct native_list *render, struct ui_rect clip,
                      struct ui_rect cell, int font, unsigned int fg,
                      const char *text, bool scroll, int offset, bool centered)
{
    struct viewport vp;
    if (!text || !*text || !list_text_ready(font >= 0 ? font : FONT_UI, text) ||
        !set_clip(&vp, clip, cell, font, fg))
        return;
    int width = font_getstringsize((const unsigned char *)text, NULL, NULL, vp.font);
    int x = cell.x - vp.x;
    int overflow = MAX(0, width - cell.width);
    if (!overflow)
    {
        if (centered)
            x += (cell.width - width) / 2;
        else if (VP_IS_RTL(render->owner->parent[SCREEN_MAIN]))
            x += cell.width - width;
    }
    if (!global_settings.offset_out_of_view)
        offset = MIN(overflow, offset);
    int period = 0;
    if (!offset && scroll && overflow && stock_marquee(render))
    {
        period = width + STOCK_MARQUEE_GAP;
        offset = stock_marquee_shift(render, period);
    }
    else if (!offset && scroll)
        offset = marquee_shift(render, overflow);
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_putsxy(x - offset, cell.y - vp.y, (const unsigned char *)text);
    if (period && offset)
        lcd_putsxy(x - offset + period, cell.y - vp.y,
                   (const unsigned char *)text);
    lcd_set_viewport(prior);
}

static void draw_bitmap(struct ui_rect clip, struct ui_rect cell,
                        const struct bitmap *bmp)
{
    struct viewport vp;
    if (!bmp || !set_clip(&vp, clip, cell, FONT_UI, 0))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_bmp(bmp, cell.x - vp.x + MAX(0, (cell.width - bmp->width) / 2),
                 cell.y - vp.y + MAX(0, (cell.height - bmp->height) / 2));
    lcd_set_viewport(prior);
}

static bool equals_lang(const char *text, int id)
{
    return text && !strcmp(text, str(id));
}

/* `generic` lets a row with no glyph of its own draw its type's mark. */
static const char *row_icon(enum themable_icons icon, enum list_row_kind kind,
                            const char *label, bool file_view,
                            enum current_activity activity, bool generic)
{
    if (file_view)
    {
        if (equals_lang(label, LANG_SEARCH)) return "®";
        if (equals_lang(label, LANG_SORT_TYPE)) return "ª";
        if (equals_lang(label, LANG_MOODS)) return "í";
        if (equals_lang(label, LANG_JOURNEYS)) return "î";
        if (equals_lang(label, LANG_PLUGIN_APPS)) return "ĥ";
        if (equals_lang(label, LANG_PLUGIN_GAMES)) return "Ħ";
        if (equals_lang(label, LANG_PLUGIN_DEMOS)) return "ħ";

        const char *ext = label ? strrchr(label, '.') : NULL;
        if (icon != Icon_Folder && ext &&
            (!strcasecmp(ext, ".mid") || !strcasecmp(ext, ".midi")))
            return "¥";

        switch (icon)
        {
            case Icon_Audio: return "¥";
            case Icon_Playlist: return "m";
            case Icon_Folder: return "¡";
            case Icon_Font:
            case Icon_Document: return "¦";
            case Icon_Image: return "§";
            case Icon_Book: return "Ę";
            case Icon_Lyrics: return "ė";
            case Icon_Video: return "ĉ";
            case Icon_Plugin: return "Ĥ";
            /* A plugin's own menu runs in a file-row activity. */
            case Icon_Menu_setting:
            case Icon_Menu_functioncall:
            case Icon_Submenu: return NULL;
            default: return icon == Icon_NOICON || !generic ? NULL : "¨";
        }
    }

    if (kind == LIST_ROW_CUSTOM || kind == LIST_ROW_CUSTOM_MENU)
        return kind == LIST_ROW_CUSTOM ? "ċ" : "Ċ";
    if (activity == ACTIVITY_SYSTEMSCREEN)
    {
        if (!strcmp(label, "PodLink")) return "@";
        if (!strcmp(label, "Agents")) return "^";
    }
    if (kind == LIST_ROW_COMMAND)
    {
        /* tagnavi group headings are tag values, not translated UI labels. */
        static const struct { const char *name, *glyph; } groups[] = {
            { "Album", "d" }, { "Single", "a" }, { "EP", "b" },
            { "Compilation", "Ĉ" }, { "Mixtape", "Ĉ" },
            { "Live Album", "Z" }, { "Soundtrack", "ĉ" },
        };
        for (unsigned i = 0; i < ARRAYLEN(groups); ++i)
            if (!strcmp(label, groups[i].name))
                return groups[i].glyph;
    }

    static const struct { int id; const char *glyph; } icons[] = {
        { LANG_SEARCH, "®" }, { LANG_SOUND_SETTINGS, "Y" },
        { LANG_PLAYBACK, "W" }, { LANG_LIBRARY, "v" },
        { LANG_APPEARANCE, "Ê" }, { LANG_BATTERY_POWER, "Á" },
        { LANG_SYSTEM, "Ò" }, { LANG_SETTINGS_MODE, "Í" },
        { LANG_SETTINGS_CHANGED, "É" }, { LANG_NOW_PLAYING, "A" },
        { LANG_RESUME_PLAYBACK, "C" }, { LANG_CONTINUE_READING, "A" },
        { LANG_MUSIC_BROWSER, "c" }, { LANG_ALBUM_COVERS, "f" },
        { LANG_ARTIST_PORTRAITS, "i" }, { LANG_CURRENT_PLAYLIST, "U" },
        { LANG_PLAYLISTS, "ð" }, { LANG_PLAYBACK_REPORT, "z" },
        { LANG_DIR_BROWSER, "¢" }, { LANG_SETTINGS, "Ç" },
        { LANG_SHORTCUTS, "Ó" }, { LANG_RANDOM_ALBUM, "V" },
        { LANG_SPUN, "µ" }, { LANG_PLAYBACK_HISTORY, "µ" },
        { LANG_SHUFFLE_SONGS, "H" }, { LANG_DOCUMENTS, "w" },
        { LANG_IMAGES, "x" }, { LANG_VIDEOS, "Ė" },
        { LANG_FEATURED_ARTISTS, "é" },
        { LANG_AUDIOBOOKS, "ê" }, { LANG_PODCASTS, "Ď" },
        { LANG_EBOOKS, "Č" }, { LANG_CUSTOM_MENU, "Ċ" },
        { LANG_ABOUT, "»" }, { LANG_ROCKBOX_INFO, "ñ" },
        { LANG_CREDITS, "ò" }, { LANG_BG_TASK_INFO, "ó" },
        { LANG_LICENSE, "ô" }, { LANG_DEBUG, "õ" },
        { LANG_TAGNAVI_ALL_TRACKS, "b" }, { LANG_TAGNAVI_RANDOM, "V" },
        { LANG_TAGNAVI_SHUFFLE_ALBUMS, "H" }, { LANG_BOOK_RESUME, "T" },
        { LANG_FEATURED_IN, "Z" }, { LANG_TAGNAVI_UNTAGGED, "¿" },
        { LANG_TAGNAVI_ALL_TRACKS_SORTED_BY_ALBUM, "d" },
        { LANG_PODCAST_ALL_EPISODES, "Ė" }, { LANG_PODCAST_PLAY_UNPLAYED, "A" },
        { LANG_BOOKS_IN_PROGRESS, "ē" }, { LANG_BOOK_SHELF_ROW_PROGRESS, "ē" },
        { LANG_BOOKS_NOT_STARTED, "đ" }, { LANG_BOOK_SHELF_ROW_NEW, "đ" },
        { LANG_BOOKS_FINISHED, "Ē" }, { LANG_BOOK_SHELF_ROW_FINISHED, "Ē" },
        { LANG_BOOK, "ë" }, { LANG_AUTHOR, "ì" }, { LANG_ID3_TITLE, "l" },
        { LANG_ID3_ALBUM, "e" }, { LANG_ALBUMS, "e" }, { LANG_ID3_ARTIST, "g" },
        { LANG_ARTISTS, "g" }, { LANG_ID3_ALBUMARTIST, "h" },
        { LANG_ID3_ALBUMARTISTS, "h" }, { LANG_ID3_GENRE, "j" },
        { LANG_ID3_YEAR, "¶" }, { LANG_ID3_COMPOSER, "k" },
        { LANG_USER_RATING, "²" }, { LANG_RECENTLY_ADDED, "´" },
        { LANG_SHOW_FIRST_LETTER, "«" }, { LANG_TRACKS_BY, "ª" },
        { LANG_SEARCH_BY, "¯" }, { LANG_SAME_AS_CURRENT, "Ø" },
        { LANG_BY_AUTHOR, "ì" }, { LANG_BY_SUBJECT, "l" },
        { LANG_READING_STATS, "Ĕ" }, { LANG_LISTENING_STATS, "Ĕ" },
        { LANG_BY_SERIES, "ģ" }, { LANG_PODCAST_BY_SHOW, "ď" },
        { LANG_QUIZ, "ï" },
    };
    if (equals_lang(label, LANG_BY_TITLE))
        return activity == ACTIVITY_BOOKSHELF ? "č" : "ë";
    for (unsigned i = 0; i < ARRAYLEN(icons); ++i)
        if (equals_lang(label, icons[i].id))
            return icons[i].glyph;

    switch (icon)
    {
        case Icon_Folder: return "m";
        case Icon_Audio: return "c";
        case Icon_Playlist: return "¥";
        case Icon_Font: return "¦";
        case Icon_Bookmark: return "Ó";
        case Icon_Plugin: return "Ĥ";
        case Icon_file_view_menu:
        case Icon_Submenu: return generic ? "Þ" : NULL;
        default: return icon == Icon_NOICON || !generic ? NULL : "¨";
    }
}

/* An Edit Main Menu row's home label: its text without the carried
 * item's '>' and the ": On" or ": Off" after it. */
static const char *home_label(const char *text, char *buffer, size_t size)
{
    strlcpy(buffer, text + (*text == '>'), size);
    char *state = strrchr(buffer, ':');
    if (state && state[1] == ' ')
        *state = '\0';
    return buffer;
}

/* The accessory of a row that opens another screen. */
static const char chevron[] = "Þ";

static const char *row_accessory(const struct gui_synclist *list, int item,
                                 enum list_row_kind kind, const char *label,
                                 const char *value)
{
    int range = list->callback_get_item_range ?
                list->callback_get_item_range(item, list->data) : -1;
    if (range >= 0)
    {
        static const char *const steps[] = { "ø", "ù", "ú", "û", "ü",
                                             "ý", "þ", "ÿ", "Ā", "ā" };
        return steps[range == 0 ? 0 : range >= 100 ? 9 : 1 + range * 8 / 100];
    }
    if (kind == LIST_ROW_TOGGLE)
    {
        if (equals_lang(value, LANG_SET_BOOL_YES))
            return "ö";
        if (equals_lang(value, LANG_SET_BOOL_NO))
            return "÷";
    }
    if (kind == LIST_ROW_SETTING)
    {
        if (equals_lang(label, LANG_BEEP))
        {
            static const struct { int id; const char *glyph; } beep_steps[] = {
                { LANG_OFF, "ø" }, { LANG_WEAK, "û" },
                { LANG_MODERATE, "þ" }, { LANG_STRONG, "ā" },
            };
            for (unsigned i = 0; i < ARRAYLEN(beep_steps); ++i)
                if (equals_lang(value, beep_steps[i].id))
                    return beep_steps[i].glyph;
        }
        if (equals_lang(value, LANG_AUTO))
            return "Ă";
    }
    if (list->callback_item_is_playing && list->callback_item_is_playing(item, list->data))
        return "A";
    return kind == LIST_ROW_MENU || kind == LIST_ROW_CONTAINER ||
           kind == LIST_ROW_CUSTOM_MENU ? chevron : NULL;
}

static bool file_rows(enum current_activity activity)
{
    return activity == ACTIVITY_FILEBROWSER || activity == ACTIVITY_PLAYLISTBROWSER ||
           activity == ACTIVITY_DOCUMENTBROWSER || activity == ACTIVITY_IMAGEBROWSER ||
           activity == ACTIVITY_VIDEOBROWSER ||
           activity == ACTIVITY_PLUGINBROWSER || activity == ACTIVITY_PLUGIN ||
           activity == ACTIVITY_FILE_SEARCH ||
           activity == ACTIVITY_FOLDERSELECT;
}

static void marker(struct native_list *render, struct ui_rect clip, struct ui_rect rect,
                   unsigned int fg)
{
    struct viewport vp;
    if (!set_clip(&vp, clip, rect, FONT_UI, fg))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    fill_round_rect_aa(&screens[SCREEN_MAIN], rect.x - vp.x, rect.y - vp.y,
                      rect.width, rect.height, MIN(rect.width / 2, 1),
                      render->mark_mask, render->theme.metric[UI_METRIC_LIST_SELECTION_ALPHA]);
    lcd_set_viewport(prior);
}

static void draw_owner_row(struct gui_synclist *list, struct ui_rect clip,
                           struct ui_rect row, int item, enum themable_icons icon,
                           bool selected, int font, unsigned int fg, unsigned int bg,
                           const char *label)
{
    struct viewport vp;
    if (!set_clip(&vp, clip, row, font, fg))
        return;
    vp.bg_pattern = bg;
    struct line_desc line = LINE_DESC_DEFINIT;
    line.height = row.height;
    line.nlines = list->selected_size;
    line.line = item < 0 ? 0 : item % MAX(1, list->selected_size);
    line.style = selected ? STYLE_INVERT : STYLE_DEFAULT;
    struct list_putlineinfo_t info = {
        .x = row.x - vp.x, .y = row.y - vp.y, .line = item, .icon = icon,
        .icon_width = get_icon_width(SCREEN_MAIN), .display = &screens[SCREEN_MAIN],
        .vp = &vp, .linedes = &line, .list = list, .dsp_text = label,
        .is_selected = selected, .is_title = item < 0,
        .show_cursor = false, .have_icons = list->show_icons && icon != Icon_NOICON,
    };
    struct viewport *prior = lcd_set_viewport(&vp);
    list->callback_draw_item(&info);
    /* Do not let a plugin retain the address of this clipped viewport. */
    lcd_scroll_stop_viewport(&vp);
    lcd_set_viewport(prior);
}

/* `lit` draws the text for the selection colour behind it; under a bar it
 * can differ from `selected` while the bar moves between rows. */
static void paint_row(struct native_list *render, struct ui_rect clip,
                      struct ui_rect row, int item, bool selected, bool lit,
                      bool marker_only)
{
    struct gui_synclist *list = render->owner;
    const uint16_t *m = render->theme.metric;
    enum current_activity activity = get_current_activity();
    enum list_row_kind kind = list->callback_get_item_kind ?
        list->callback_get_item_kind(item, list->data) : LIST_ROW_PLAIN;
    enum themable_icons icon = list->callback_get_item_icon ?
        list->callback_get_item_icon(item, list->data) : Icon_NOICON;
    bool page_menu = menu_page_list(list);
    char own_glyph[4] = "";
    if (ICON_IS_GLYPH(icon))
    {
        *utf8encode(ICON_GLYPH_CODE(icon), (unsigned char *)own_glyph) = '\0';
        icon = Icon_NOICON;
    }

    /* Edit Main Menu shows each home row as home draws it, wherever it runs */
    bool menu_config = equals_lang(list->title, LANG_MAIN_MENU_SETTINGS);
    bool files = file_rows(activity) && !menu_config;
    bool main_settings = activity == ACTIVITY_SETTINGS && equals_lang(list->title, LANG_SETTINGS);
    bool database = activity == ACTIVITY_DATABASEBROWSER;
    bool library_menu = database && (kind == LIST_ROW_MENU || kind == LIST_ROW_ACTION) &&
        (equals_lang(list->title, LANG_MUSIC_BROWSER) || equals_lang(list->title, LANG_AUDIOBOOKS) ||
         equals_lang(list->title, LANG_PODCASTS));
    bool books_menu = activity == ACTIVITY_BOOKSHELF && equals_lang(list->title, LANG_EBOOKS);
    bool agents = activity == ACTIVITY_AGENTS && list->callback_draw_status;
    bool art = list->callback_get_item_albumart != NULL;
    int icon_size = m[UI_METRIC_LIST_ICON_SIZE];
    int margin = m[UI_METRIC_LIST_ICON_MARGIN];
    int text_x = m[UI_METRIC_LIST_TEXT_PADDING];
    bool icon_column = list->show_icons && (files || main_settings || activity == ACTIVITY_SYSTEMSCREEN ||
        library_menu || books_menu || agents || activity == ACTIVITY_MAINMENU || menu_config ||
        (database && (kind == LIST_ROW_CUSTOM || kind == LIST_ROW_CUSTOM_MENU ||
         equals_lang(list->title, LANG_CUSTOM_MENU))));
    if (!icon_column && list->show_icons && database && kind == LIST_ROW_COMMAND)
    {
        char *name = render->scratch.row.name;
        const unsigned char *label_ptr = (const unsigned char *)
            list->callback_get_item_name(item, list->data, name, sizeof(render->scratch.row.name));
        const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER
                            ? P2STR(label_ptr) : "";
        icon_column = !equals_lang(label, LANG_TAGNAVI_ALL_TRACKS) &&
                      !equals_lang(label, LANG_TAGNAVI_ALL_TRACKS_SORTED_BY_ALBUM);
    }
    /* Like a native menu, a plugin's menu has an icon column only for a row
     * that has a glyph of its own. */
    if (files && list->show_icons &&
        (kind == LIST_ROW_ACTION || kind == LIST_ROW_MENU ||
         kind == LIST_ROW_SETTING || kind == LIST_ROW_TOGGLE))
    {
        char *name = render->scratch.row.name;
        const unsigned char *label_ptr = (const unsigned char *)
            list->callback_get_item_name(item, list->data, name, sizeof(render->scratch.row.name));
        const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER
                            ? P2STR(label_ptr) : "";
        icon_column = row_icon(icon, kind, label, true, activity, true) != NULL;
    }
    icon_column |= list->show_icons && page_menu;
    int marked = list_select_row_state(list, item);
    if (!icon_column && !art && list->show_icons &&
        (kind == LIST_ROW_CONTAINER || kind == LIST_ROW_CUSTOM_MENU))
        icon_column = icon != Icon_NOICON;
    /* Agents keeps its column: it holds each agent's live state. */
    if (render->theme.style[UI_STYLE_LIST_ICONS] == UI_LIST_ICONS_OFF &&
        !agents)
        icon_column = false;

    if (marker_only)
    {
        if (art)
            text_x += MIN(m[UI_METRIC_LIST_ART_SIZE], row.height) +
                      m[UI_METRIC_LIST_AUX_PADDING] + 1;
        else if (icon_column || marked >= 0)
            text_x += icon_size + m[UI_METRIC_LIST_ICON_GAP];
        int x = text_x - m[UI_METRIC_LIST_TEXT_PADDING] + margin;
        struct ui_rect rect = row_cell(row, x, 0,
                                       m[UI_METRIC_LIST_SELECTION_WIDTH],
                                       row.height,
                                       VP_IS_RTL(list->parent[SCREEN_MAIN]));
        marker(render, clip, rect, render->colors[UI_COLOR_SELECTION]);
        struct ui_rect *column = &render->shown.marker;
        if (column->width > 0)
            rect = bounding(*column, rect);
        *column = (struct ui_rect){ rect.x, 0, rect.width, 0 };
        return;
    }

    char *name = render->scratch.row.name;
    const unsigned char *label_ptr = (const unsigned char *)list->callback_get_item_name(
        item, list->data, name, sizeof(render->scratch.row.name));
    const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER ?
                        (const char *)P2STR(label_ptr) : "";
    const char *value = list->callback_get_item_value ?
                        list->callback_get_item_value(item, list->data, render->scratch.row.value,
                                                      sizeof(render->scratch.row.value)) : NULL;
    const char *icon_label = menu_config ? home_label(label, render->scratch.row.home,
                                                      sizeof(render->scratch.row.home)) : label;
    bool chevron_selected =
        render->theme.style[UI_STYLE_ACCESSORY] == UI_ACCESSORY_SELECTED;
    const char *aux = menu_config ||
                      (activity == ACTIVITY_MAINMENU && !chevron_selected) ? NULL :
                      row_accessory(list, item, kind, label, value);
    /* Home's rows open screens, except the ones that start playback. */
    if (activity == ACTIVITY_MAINMENU && chevron_selected && !aux &&
        !equals_lang(label, LANG_RESUME_PLAYBACK) &&
        !equals_lang(label, LANG_SHUFFLE_SONGS) &&
        !equals_lang(label, LANG_RANDOM_ALBUM))
        aux = chevron;
    /* With stock accessories a setting shows its value in words instead. */
    bool value_text = chevron_selected && value && *value &&
                      (kind == LIST_ROW_SETTING || kind == LIST_ROW_TOGGLE);
    if (value_text)
        aux = NULL;

    unsigned int fg = render->colors[lit ? UI_COLOR_SELECTED_TEXT : UI_COLOR_MUTED];
    unsigned int bg = render->colors[UI_COLOR_BACKGROUND];
    if (list->callback_get_item_color)
    {
        int row_color = list->callback_get_item_color(item, list->data);
        if (row_color >= 0)
            fg = (unsigned int)row_color;
    }
    if (list->selection_color && selected)
        fg = list->selection_color->text_color;
    int text_font = render->text_font >= 0 ? render->text_font : list->parent[SCREEN_MAIN]->font;
    if (list->callback_draw_item)
    {
        if (list_text_ready(text_font, label))
            draw_owner_row(list, clip, row, item, icon, selected, text_font, fg, bg, label);
        return;
    }

    bool now_playing_row = activity == ACTIVITY_MAINMENU &&
                           equals_lang(label, LANG_NOW_PLAYING);
    /* With stock accessories the row reads a plain "Now Playing", as
     * stock's does. */
    if (now_playing_row && !chevron_selected)
    {
        const struct mp3entry *id3 = audio_current_track();
        if (id3 && id3->title && *id3->title)
        {
            char *now_playing = render->scratch.row.playing;
            snprintf(now_playing, sizeof(render->scratch.row.playing), "%s: %s", label, id3->title);
            label = now_playing;
        }
    }

    bool ready = list_text_ready(text_font, label);
    if (agents && value && value[0] == 'r' && value[1] && value[2])
        ready &= list_text_ready(text_font, value + 2);
    int value_font = render->value_font >= 0 ? render->value_font : text_font;
    if (value_text)
        ready &= list_text_ready(value_font, value);
    if (!ready)
        return;

    bool rtl = VP_IS_RTL(list->parent[SCREEN_MAIN]);
    int icon_y = (row.height - icon_size) / 2;
    struct ui_rect icon_cell = row_cell(row, margin, icon_y, icon_size, icon_size, rtl);
    if (art)
    {
        int cover_size = MIN(m[UI_METRIC_LIST_ART_SIZE], row.height);
        struct dim size = {cover_size, cover_size};
        const struct bitmap *cover = list->callback_get_item_albumart(
            item, list->data, &size);
        struct ui_rect tile = row_cell(row, 0, (row.height - cover_size) / 2,
                                        cover_size, cover_size, rtl);
        if (cover)
            draw_bitmap(clip, tile, cover);
        else if (list->callback_prefetch_art)
            fill(clip, tile, LCD_RGBPACK(45, 45, 45));
        if (!cover && !list->callback_prefetch_art && render->icon_font >= 0)
            draw_text(render, clip, icon_cell, render->icon_font, fg,
                      row_icon(icon, kind, icon_label, files, activity, true),
                      false, 0, false);
        text_x += cover_size + m[UI_METRIC_LIST_AUX_PADDING] + 1;
    }
    else if (icon_column || marked >= 0)
    {
        text_x += icon_size + m[UI_METRIC_LIST_ICON_GAP];
        if (render->icon_font >= 0)
        {
            const char *glyph = marked >= 0 ? (marked ? "×" : "Ö") :
                                *own_glyph ? own_glyph :
                                row_icon(icon, kind, icon_label, files,
                                         activity, !page_menu);
            if (now_playing_row && marked < 0)
                glyph = audio_status() & AUDIO_STATUS_PAUSE ? "B" : "A";
            if (agents && value && value[1] && (value[0] == 'r' || value[0] == 'q'))
            {
                static const char agent_icons[] = "alscx";
                static const char *const glyphs[] = {"!", "/", "=", "Ö", "×"};
                const char *at = strchr(agent_icons, value[1]);
                glyph = at ? glyphs[at - agent_icons] : NULL;
            }
            draw_text(render, clip, icon_cell, render->icon_font, fg,
                      glyph, false, 0, false);
        }
    }

    if (list->callback_draw_status)
    {
        struct viewport vp;
        if (set_clip(&vp, clip, icon_cell, text_font, fg))
        {
            vp.bg_pattern = bg;
            struct viewport *prior = lcd_set_viewport(&vp);
            list->callback_draw_status(item, list->data, &screens[SCREEN_MAIN], &vp);
            lcd_set_viewport(prior);
        }
    }

    int right = row.width - m[UI_METRIC_LIST_RIGHT_PADDING];
    int arrow = right - icon_size;
    int text_height = MIN(row.height, m[UI_METRIC_LIST_TEXT_HEIGHT]);
    int text_y = (row.height - text_height) / 2;
    /* With stock accessories a row's text keeps the text padding clear at
     * its right end, or stops short of the chevron, which keeps 6 columns
     * clear of the list's right edge, the scroll indicator's included. */
    if (aux == chevron && chevron_selected && !lit)
        aux = NULL;
    const struct stock_art *glyph = &stock_art[STOCK_ART_SYSTEM_SUBMENU];
    int chevron_x = right + render->row_inset - 6 - glyph->width;
    bool art_chevron = aux == chevron && !rtl && ui_theme_art(&render->theme);
    int text_right = right;
    if (aux == chevron && chevron_selected)
        text_right = chevron_x - m[UI_METRIC_LIST_ACCESSORY_GAP];
    else if (aux)
        text_right = arrow - m[UI_METRIC_LIST_ACCESSORY_GAP];
    else if (chevron_selected)
        text_right = right - m[UI_METRIC_LIST_TEXT_PADDING];
    if (agents && value && value[0] == 'r' && value[1] && value[2])
    {
        int value_width = 4 * text_height;
        text_right -= value_width + m[UI_METRIC_LIST_AUX_PADDING];
        draw_text(render, clip, row_cell(row, right - value_width, text_y,
                  value_width, text_height, rtl), text_font, fg, value + 2, false, 0, false);
    }
    if (value_text)
    {
        int inset = m[UI_METRIC_LIST_TEXT_PADDING];
        int value_width = MIN(font_getstringsize((const unsigned char *)value,
                                                 NULL, NULL, value_font),
                              (right - text_x) / 2);
        int value_x = right - inset - value_width;
        /* The value shares the label's baseline. */
        int value_y = text_y + font_get(text_font)->ascent -
                      font_get(value_font)->ascent;
        text_right = value_x - m[UI_METRIC_LIST_AUX_PADDING];
        draw_text(render, clip, row_cell(row, value_x, value_y, value_width,
                  text_height, rtl), value_font,
                  lit ? fg : render->colors[UI_COLOR_ACCENT], value, false, 0,
                  false);
    }
    bool scroll = selected || list->scroll_all;
    char fitted[TEXT_FIT_BUF];
    /* Under the bar, as in the stock firmware, a row that does not scroll
     * ends its cut label in "...". */
    if (!scroll && !list->offset_position[SCREEN_MAIN] &&
        render->theme.style[UI_STYLE_SELECTION] == UI_SELECTION_BAR)
        label = text_fit(label, fitted, sizeof(fitted), text_font,
                         text_right - text_x);
    draw_text(render, clip, row_cell(row, text_x, text_y,
              text_right - text_x, text_height, rtl), text_font, fg, label,
              scroll, list->offset_position[SCREEN_MAIN], false);
    if (art_chevron)
    {
        struct ui_rect cell = row_cell(row, chevron_x,
                                       (row.height - glyph->height) / 2,
                                       glyph->width, glyph->height, false);
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU, cell.x, cell.y,
                       intersection(clip, cell));
        aux = NULL;
    }
    if (render->icon_font >= 0)
        draw_text(render, clip, row_cell(row, arrow, icon_y, icon_size, icon_size, rtl),
                  render->icon_font, fg, aux, false, 0, false);
}

static int scrollbar_width(const struct gui_synclist *list, struct ui_rect bounds,
                           int visible)
{
    if (visible <= 0 || list->nb_items <= visible || list->scrollbar == SCROLLBAR_OFF)
        return 0;
    return MIN(MAX(global_settings.scrollbar_width, 1), bounds.width / 4);
}

/* The classic scroll indicator always sits at the right edge. */
static bool scrollbar_left(const struct native_list *render,
                           const struct gui_synclist *list)
{
    return list->scrollbar == SCROLLBAR_LEFT &&
           render->theme.style[UI_STYLE_SCROLLBAR] != UI_SCROLLBAR_CLASSIC;
}

/* `opacity` at `level` of INDICATOR_STEPS strength. */
static unsigned faded(unsigned opacity, int level)
{
    return opacity * level / INDICATOR_STEPS;
}

static void blend(struct ui_rect clip, struct ui_rect cell, unsigned int color,
                  unsigned opacity)
{
    struct viewport vp;
    if (!set_clip(&vp, clip, cell, FONT_UI, color))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_blendrect(0, 0, vp.width, vp.height, opacity);
    lcd_set_viewport(prior);
}

/* `id` at (x, y) as stock_art_draw() draws it, at `level` of
 * INDICATOR_STEPS strength. */
static void draw_art_faded(enum stock_art_id id, int x, int y,
                           struct ui_rect clip, int level)
{
    const struct stock_art *art = &stock_art[id];
    if (level >= INDICATOR_STEPS)
    {
        stock_art_draw(id, x, y, clip);
        return;
    }
    clip = intersection(clip, (struct ui_rect){ x, y, art->width,
                                                art->height });
    clip = intersection(clip, (struct ui_rect){ 0, 0, LCD_WIDTH,
                                                LCD_HEIGHT });
    if (clip.width <= 0 || clip.height <= 0 || clip.width > 16)
        return;
    for (int row = clip.y; row < clip.y + clip.height; ++row)
    {
        fb_data *out = lcd_framebuffer_default.fb_ptr + row * LCD_WIDTH +
                       clip.x;
        fb_data mixed[16];
        memcpy(mixed, out, clip.width * sizeof(*out));
        stock_art_row(id, clip.x - x, row - y, mixed, clip.width);
        for (int i = 0; i < clip.width; ++i)
        {
            int c[3] = { FB_UNPACK_RED(out[i]), FB_UNPACK_GREEN(out[i]),
                         FB_UNPACK_BLUE(out[i]) };
            int m[3] = { FB_UNPACK_RED(mixed[i]), FB_UNPACK_GREEN(mixed[i]),
                         FB_UNPACK_BLUE(mixed[i]) };
            for (int k = 0; k < 3; ++k)
                c[k] += (m[k] - c[k]) * level / INDICATOR_STEPS;
            out[i] = FB_RGBPACK(c[0], c[1], c[2]);
        }
    }
}

/* Stock's own indicator: its top and bottom bitmaps with the middle one
 * repeated between, against the bar's right edge, in a track 2 rows short
 * of the bar at each end. Its length is the visible share of the track. */
static void paint_art_scrollbar(struct ui_rect clip, struct ui_rect bar,
                                int items, int first, int last, int level)
{
    const struct stock_art *top = &stock_art[STOCK_ART_SYSTEM_SCROLLBARTOP];
    int bottom = stock_art[STOCK_ART_SYSTEM_SCROLLBARBOTTOM].height;
    int middle = stock_art[STOCK_ART_SYSTEM_SCROLLBARCENTER].height;
    int track = bar.height - 4;
    if (track < top->height + bottom || items <= 0)
        return;
    first = MIN(MAX(first, 0), items);
    last = MIN(MAX(last, first), items);
    int length = (int64_t)track * (last - first) / items;
    length = MAX(length, top->height + bottom);
    int span = items - (last - first);
    int y = bar.y + 2 +
            (span > 0 ? (int64_t)(track - length) * first / span : 0);
    int x = bar.x + bar.width - top->width;
    int end = y + length - bottom;
    struct ui_rect body = intersection(clip, (struct ui_rect){
        x, y + top->height, top->width, end - y - top->height });
    for (int row = y + top->height; row < end; row += middle)
        draw_art_faded(STOCK_ART_SYSTEM_SCROLLBARCENTER, x, row, body, level);
    draw_art_faded(STOCK_ART_SYSTEM_SCROLLBARTOP, x, y, clip, level);
    draw_art_faded(STOCK_ART_SYSTEM_SCROLLBARBOTTOM, x, end, clip, level);
}

/* The stock indicator: no track, a translucent foreground thumb with rounded
 * ends inside a faint background rim, laid over the rows. gradient.scrollbar
 * instead shades an opaque thumb across its width. Items run [first, last)
 * of `items`, in any unit. */
static void paint_classic_scrollbar(struct native_list *render,
                                    struct ui_rect clip, struct ui_rect bar,
                                    int items, int first, int last, int level)
{
    if (ui_theme_art(&render->theme))
    {
        paint_art_scrollbar(clip, bar, items, first, last, level);
        return;
    }
    struct ui_rect track = { bar.x + 1, bar.y + 3, bar.width - 2,
                             bar.height - 6 };
    if (track.width <= 0 || track.height <= 0 || items <= 0)
        return;
    first = MIN(MAX(first, 0), items);
    last = MIN(MAX(last, first), items);
    int top = (int64_t)first * track.height / items;
    int bottom = (int64_t)last * track.height / items;
    int min_height = MIN(track.height, 2 * track.width);
    if (bottom - top < min_height)
    {
        top = MIN(top, track.height - min_height);
        bottom = top + min_height;
    }
    struct ui_rect thumb = { track.x, track.y + top, track.width,
                             bottom - top };

    if (render->theme.gradient[UI_GRADIENT_SCROLLBAR].stops)
    {
        const uint16_t *shade = gradient_rows(render, &render->thumb_rows,
                                              UI_GRADIENT_SCROLLBAR,
                                              thumb.width);
        struct ui_rect column = { thumb.x, thumb.y, 1, thumb.height };
        for (int x = 0; x < thumb.width; ++x, ++column.x)
            fill(clip, column, shade[x]);
        return;
    }

    unsigned int rim = render->colors[UI_COLOR_BACKGROUND];
    unsigned int ink = render->colors[UI_COLOR_FOREGROUND];
    int x = thumb.x, y = thumb.y, w = thumb.width, h = thumb.height;
    unsigned edge_opacity = faded(2, level);
    blend(clip, (struct ui_rect){ x - 1, y - 1, w + 2, 1 }, rim, edge_opacity);
    blend(clip, (struct ui_rect){ x - 1, y + h, w + 2, 1 }, rim, edge_opacity);
    blend(clip, (struct ui_rect){ x - 1, y, 1, h }, rim, edge_opacity);
    blend(clip, (struct ui_rect){ x + w, y, 1, h }, rim, edge_opacity);
    /* The two rows at each end fade towards the corners. */
    static const uint8_t corner[2] = { 1, 4 }, middle[2] = { 5, 7 };
    for (int edge = 0; edge < 2 && 2 * edge < h; ++edge)
        for (int end = 0; end < 2; ++end)
        {
            int row = end ? y + h - 1 - edge : y + edge;
            blend(clip, (struct ui_rect){ x, row, 1, 1 }, ink,
                  faded(corner[edge], level));
            blend(clip, (struct ui_rect){ x + w - 1, row, 1, 1 }, ink,
                  faded(corner[edge], level));
            blend(clip, (struct ui_rect){ x + 1, row, w - 2, 1 }, ink,
                  faded(middle[edge], level));
            if (2 * edge + 1 == h)
                break;
        }
    blend(clip, (struct ui_rect){ x, y + 2, w, h - 4 }, ink, faded(7, level));
}

/* Whether the list shows stock's letter tile: the themes with stock's look. */
static bool stock_index(const struct native_list *render)
{
    return render->theme.style[UI_STYLE_WPS_LAYOUT] == UI_WPS_LAYOUT_CLASSIC;
}

static void paint_rows(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    struct gui_synclist *list = render->owner;
    int pitch = row_pitch(list, SCREEN_MAIN, render->theme.metric);
    int visible = frame->bounds.height / pitch;
    int spare = row_spare(&render->theme, frame->bounds.height, pitch);
    struct ui_rect content = frame->bounds;
    content.height = visible * pitch + spare;
    struct ui_rect clip = intersection(frame->clip, content);
    if (clip.width <= 0 || clip.height <= 0)
        return;

    int first = MAX(frame->scroll_offset, 0);
    int end = MIN(frame->scroll_offset + visible + (render->row_shift != 0),
                  list->nb_items);
    struct ui_rect row = content;
    int rows_top = content.y + render->row_shift;
    int bar_width = scrollbar_width(list, frame->bounds, visible);
    /* The classic indicator lies over the rows for a moment after a move;
     * the rows keep their width. */
    bool overlay = classic_indicator(render);
    int level = overlay && bar_width ? render->indicator : 0;
    if (!overlay && scrollbar_left(render, list))
        row.x += bar_width;
    if (!overlay)
        row.width -= bar_width;
    render->row_inset = overlay || scrollbar_left(render, list) ? 0 : bar_width;

    /* Stock hides the selection while its letter tile shows. */
    bool unlit = render->overlay.visible && stock_index(render);
    struct ui_rect highlight = {overlay ? content.x : row.x, render->cursor_y,
                                overlay ? content.width : row.width,
                                render->cursor_height};
    if (list->selection_color && list->nb_items && !unlit)
        fill(clip, highlight, list->selection_color->line_color);

    bool selection_bar =
        render->theme.style[UI_STYLE_SELECTION] == UI_SELECTION_BAR &&
        !list->selection_color && !list->callback_draw_item;
    /* Under a bar, the rows above and below it take plain text and the band
     * the bar covers takes selected text, so the text follows a moving bar. */
    struct ui_rect bands[3] = { clip };
    int nbands = 1;
    if (selection_bar && !unlit && frame->selection >= 0 &&
        frame->selection < list->nb_items)
    {
        const struct ui_theme_gradient *gradient =
            &render->theme.gradient[UI_GRADIENT_SELECTION];
        if (gradient->stops && highlight.height <= LCD_HEIGHT)
            fill_rows(clip, highlight, gradient_rows(render, &render->bar_rows,
                      UI_GRADIENT_SELECTION, highlight.height));
        else
            fill(clip, highlight, render->colors[UI_COLOR_SELECTION]);
        int top = MIN(MAX(highlight.y, clip.y), clip.y + clip.height);
        int bottom = MIN(MAX(highlight.y + highlight.height, top),
                         clip.y + clip.height);
        bands[0] = (struct ui_rect){clip.x, clip.y, clip.width, top - clip.y};
        bands[1] = (struct ui_rect){clip.x, top, clip.width, bottom - top};
        bands[2] = (struct ui_rect){clip.x, bottom, clip.width,
                                    clip.y + clip.height - bottom};
        nbands = 3;
    }

    for (int item = first; item < end; ++item)
    {
        int slot = item - frame->scroll_offset;
        row.y = rows_top + slot_y(slot, pitch, spare);
        row.height = slot_y(slot + 1, pitch, spare) - (row.y - rows_top);
        bool selected = !unlit && item >= frame->selection &&
                        item - frame->selection < list->selected_size;
        bool deferred = text_deferred;
        row_skipped = 0;
        row_hold = 0;
        const struct list_shown *shown = &render->shown;
        int old = item - shown->top;
        if (render->holding && old >= 0 &&
            old < MIN(shown->slots, SHOWN_ROWS) && shown->skipped[old])
        {
            if (render->repair_rows >= row.height)
                render->repair_rows -= row.height;
            else
            {
                row_hold = shown->skipped[old];
                render->repairing = true;
            }
        }
        for (int band = 0; band < nbands; ++band)
            if (intersection(row, bands[band]).height > 0)
            {
                row_texts = 0;
                paint_row(render, bands[band], row, item, selected,
                          selection_bar ? band == 1 : selected, false);
            }
        text_deferred = deferred;
        row_hold = 0;
        struct ui_rect painted = intersection(row, clip);
        if (painted.height <= 0)
            continue;
        render->slots = MAX(render->slots, slot + 1);
        if (slot >= SHOWN_ROWS)
            text_deferred |= row_skipped != 0;
        else if (!memcmp(&painted, &row, sizeof(row)))
            render->skipping[slot] = row_skipped;
        else
            render->skipping[slot] |= row_skipped;
    }

    /* The bar can be half visible even if its corresponding text is clipped. */
    if (!selection_bar && !list->selection_color && !list->callback_draw_item &&
        !unlit)
    {
        for (int line = 0; line < list->selected_size; ++line)
        {
            int item = frame->selection + line;
            if (item < 0 || item >= list->nb_items)
                break;
            row.y = render->cursor_y + line * pitch;
            row.height = pitch;
            if (intersection(row, clip).height > 0)
                paint_row(render, clip, row, item, true, true, true);
        }
    }

    if (bar_width && (!overlay || level))
    {
        int bar_x = scrollbar_left(render, list) ? frame->bounds.x :
                    frame->bounds.x + frame->bounds.width - bar_width;
        struct ui_rect bar = {bar_x, content.y, bar_width, content.height};
        struct viewport vp;
        unsigned int fg = render->colors[UI_COLOR_MUTED];
        if (set_clip(&vp, clip, bar, render->text_font, fg))
        {
            /* A physical pull shortens the thumb when its range clips at the end. */
            int units = MIN(ROW_UNIT, INT_MAX / MAX(1, bar.height) / list->nb_items);
            units = MAX(1, units);
            int start = frame->scroll_offset * units - render->row_shift * units / pitch;
            if (overlay)
                paint_classic_scrollbar(render, clip, bar, list->nb_items * units,
                                        start, start + visible * units, level);
            else
            {
                struct viewport *prior = lcd_set_viewport(&vp);
                gui_scrollbar_draw(&screens[SCREEN_MAIN], bar.x - vp.x, bar.y - vp.y,
                                  bar.width, bar.height, list->nb_items * units, start,
                                  start + visible * units, VERTICAL | FOREGROUND);
                lcd_set_viewport(prior);
            }
        }
    }
}

static void paint_background(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct native_list *render = view->data;
    unsigned int bg = render->colors[UI_COLOR_BACKGROUND];
    if (render->owner->selection_color)
        bg = render->owner->selection_color->bg_color;
    fill(frame->clip, frame->bounds, bg);
}

static void paint_title(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    struct gui_synclist *list = render->owner;
    unsigned int fg = list->title_color >= 0 ? (unsigned int)list->title_color :
                      render->colors[UI_COLOR_FOREGROUND];
    if (list->callback_draw_item)
    {
        draw_owner_row(list, frame->clip, frame->bounds, -1, list->title_icon, false,
                       render->title_font, fg, render->colors[UI_COLOR_BACKGROUND],
                       list->title);
        return;
    }
    struct ui_rect text = frame->bounds;
    int pad = render->theme.metric[UI_METRIC_LIST_TEXT_PADDING];
    text.x += pad;
    text.width -= 2 * pad;
    int height = MIN(text.height, render->theme.metric[UI_METRIC_LIST_TEXT_HEIGHT]);
    text.y += (text.height - height) / 2;
    text.height = height;
    draw_text(render, frame->clip, text, render->title_font, fg, list->title,
              true, 0, false);
}

static void paint_album_header(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    const struct list_header *header = render->owner->header;
    if (!header)
        return;
    const uint16_t *m = render->theme.metric;
    int padding = m[UI_METRIC_LIST_AUX_PADDING];
    struct ui_rect cell = frame->bounds;
    cell.x += padding;
    cell.width -= 2 * padding;
    if (header->cover)
    {
        int size_px = MIN(m[UI_METRIC_LIST_ART_SIZE], cell.height);
        struct dim size = { size_px, size_px };
        const struct bitmap *cover = header->cover(header->data, &size);
        struct ui_rect tile = {cell.x, cell.y, size_px, size_px};
        draw_bitmap(frame->clip, tile, cover);
        cell.x += size_px + m[UI_METRIC_LIST_ICON_GAP];
        cell.width -= size_px + m[UI_METRIC_LIST_ICON_GAP];
    }
    cell.height = m[UI_METRIC_LIST_TEXT_HEIGHT];
    draw_text(render, frame->clip, cell, render->title_font,
              render->colors[UI_COLOR_FOREGROUND], header->title, false, 0, false);
    cell.y += cell.height;
    draw_text(render, frame->clip, cell, render->text_font,
              render->colors[UI_COLOR_MUTED], header->subtitle, false, 0, false);
    cell.y += cell.height;
    draw_text(render, frame->clip, cell, render->text_font,
              render->colors[UI_COLOR_MUTED], header->detail, false, 0, false);
}

static void paint_notes(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    struct gui_synclist *list = render->owner;
    if (!list->nb_items || !list->callback_get_item_description)
        return;
    const char *text = list->callback_get_item_description(list->selected_item,
                          list->data, render->scratch.notes, sizeof(render->scratch.notes));
    if (!text || !*text)
        return;
    if (!list_text_ready(render->text_font, text) ||
        !list_text_ready(render->text_font, "."))
        return;

    int pad = render->theme.metric[UI_METRIC_LIST_AUX_PADDING];
    struct ui_rect cell = frame->bounds;
    unsigned int fg = render->colors[UI_COLOR_FOREGROUND];
    unsigned int panel = render->colors[UI_COLOR_LIST_TINT];
    int alpha = render->theme.metric[UI_METRIC_LIST_TINT_ALPHA];
    if (alpha >= 15)
        fill(frame->clip, cell, panel);
    else if (alpha > 0)
    {
        struct viewport panel_vp;
        if (set_clip(&panel_vp, frame->clip, cell, render->text_font, panel))
        {
            struct viewport *prior = lcd_set_viewport(&panel_vp);
            lcd_blendrect(0, 0, panel_vp.width, panel_vp.height, alpha);
            lcd_set_viewport(prior);
        }
    }
    cell.x += pad;
    cell.y += pad;
    cell.width -= 2 * pad;
    cell.height -= 2 * pad;
    struct viewport vp;
    if (!set_clip(&vp, frame->clip, cell, render->text_font, fg))
        return;
    struct viewport *prior = lcd_set_viewport(&vp);
    int font_height = font_get(vp.font)->height;
    int overflow = text_wrap_count(text, cell.width, vp.font) * font_height - cell.height;
    int offset = marquee_shift(render, MAX(0, overflow));
    text_wrap_draw(&screens[SCREEN_MAIN], text, cell.x - vp.x,
                   cell.y - vp.y - offset, cell.width, 'l');
    lcd_set_viewport(prior);
}

/* A row's key: the list's own when it has one (a year, a track number, or
 * a letter its sort chose), otherwise stock's key of the row's name. */
static void row_key(int item, char *key, void *ctx)
{
    struct gui_synclist *list = ctx;
    char name[MAX_PATH];

    if (item < 0 || item >= list->nb_items)
        return;
    if (list->callback_get_index_key)
    {
        const char *own = list->callback_get_index_key(item, list->data, name,
                                                       sizeof(name));
        if (own && *own)
        {
            strlcpy(key, own, QUICKSCROLL_KEY_SIZE);
            return;
        }
    }
    if (!list->callback_get_item_name)
        return;
    const unsigned char *label = (const unsigned char *)
        list->callback_get_item_name(item, list->data, name, sizeof(name));
    quickscroll_key_of(label && P2ID(label) <= VOICEONLY_DELIMITER ?
                       (const char *)P2STR(label) : "", key);
}

#if !defined(IPOD_6G) || defined(SIMULATOR)
static uint32_t index_wheel_at;
#endif

/* Whether a finger is on the wheel. A wheel with no touch sensor counts as
 * touched for half a second after each notch. */
static bool index_touched(void)
{
#if defined(IPOD_6G) && !defined(SIMULATOR)
    return wheel_status() >= 0;
#else
    return USEC_TIMER - index_wheel_at < 500000;
#endif
}

static struct quickscroll_list index_rows(struct native_list *render,
                                          struct gui_synclist *list)
{
    return (struct quickscroll_list){
        .count = list->nb_items, .selected = list->selected_item,
        .row_height = row_pitch(list, SCREEN_MAIN, render->theme.metric) *
                      MAX(1, list->selected_size),
        .key = row_key, .ctx = list,
    };
}

static void index_release_fonts(struct native_list *render);

/* A list's wheel state lasts while its contents do, as each stock screen
 * keeps its own. */
static void index_own(struct native_list *render, struct gui_synclist *list)
{
    struct list_index *index = &render->overlay;
    if (index->list == list && index->generation == list->generation)
        return;
    quickscroll_reset(&index->qs);
    index->list = list;
    index->generation = list->generation;
    index->visible = false;
    index->key[0] = '\0';
    render->index_shown[0] = '\0';
}

static void index_refresh(struct native_list *render, struct gui_synclist *list)
{
    struct list_index *index = &render->overlay;
    struct quickscroll_list rows = index_rows(render, list);
    index->visible = quickscroll_overlay(&index->qs, &rows, index->key);
    if (!index->visible)
        render->index_shown[0] = '\0';
}

/* Asks for the text of the groups either side of the selection, so the
 * next jump draws its rows on the frame it lands. */
static void index_prefetch_groups(struct native_list *render,
                                  struct gui_synclist *list,
                                  struct quickscroll_list *rows)
{
    int font = render->text_font >= 0 ? render->text_font :
                                        list->parent[SCREEN_MAIN]->font;
    if (!list->callback_get_item_name)
        return;
    for (int d = -1; d <= 1; d += 2)
    {
        int start = quickscroll_group(rows, list->selected_item, d);
        int end = MIN(list->nb_items, start + INDEX_PREFETCH_ROWS);
        for (int i = start; i < end; i++)
        {
            const unsigned char *label = (const unsigned char *)
                list->callback_get_item_name(i, list->data,
                                             render->scratch.row.name,
                                             sizeof(render->scratch.row.name));
            if (label && P2ID(label) <= VOICEONLY_DELIMITER)
                font_prefetch_text(font, P2STR(label));
        }
    }
}

bool list_render_wheel(struct gui_synclist *list, int positions, int *rows)
{
    *rows = 0;
    if (renderer_handle <= 0)
    {
        *rows = positions / 4;
        return false;
    }
    struct native_list *render = core_get_data_pinned(renderer_handle);
    struct list_index *index = &render->overlay;
    refresh_tokens(render);
    index_own(render, list);
    uint32_t now = USEC_TIMER;
#if !defined(IPOD_6G) || defined(SIMULATOR)
    index_wheel_at = now;
#endif
    struct quickscroll_list in = index_rows(render, list);
    /* The wheel reports each position it passes at the speeds where
     * stock's thresholds lie, so each is a report. */
    quickscroll_moved(&index->qs, now, abs(positions));
    struct quickscroll_move move = quickscroll_wheel(&index->qs, &in, now,
                                                     positions, index_touched());
    bool handled = !move.rows && (index->qs.mode || move.jumped);
    *rows = move.rows;
    if (handled)
    {
        motion_stop(&render->motion);
        gui_synclist_select_item(list, move.selected);
        if (move.jumped)
        {
            gui_synclist_set_top_item(list, move.selected);
            in.selected = move.selected;
            index_prefetch_groups(render, list, &in);
        }
    }
    index_refresh(render, list);
    core_put_data_pinned(render);
    if (handled)
        gui_synclist_draw(list);
    return handled;
}

static int index_face(struct native_list *render, int i);

/* The overlay of the list being drawn: it closes when the finger leaves
 * the wheel. A list long enough to jump has its face loaded ahead, as a
 * jump needs more than a second of spinning first. */
static bool step_index(struct native_list *render)
{
    struct gui_synclist *list = render->owner;
    struct list_index *index = &render->overlay;
    index_own(render, list);
    if (list->nb_items >= INDEX_PRELOAD_ROWS)
        index_face(render, stock_index(render) ? 2 : 0);
    if (index->qs.mode && !index_touched())
    {
        quickscroll_release(&index->qs);
        index_refresh(render, list);
    }
    return index->visible;
}

static void index_release_fonts(struct native_list *render)
{
    for (unsigned i = 0; i < ARRAYLEN(render->index_faces); i++)
    {
        font_unload(render->index_faces[i]);
        render->index_faces[i] = -1;
    }
    render->index_shown[0] = '\0';
}

/* The overlay's faces: Scrim's letters, its figures for longer labels, and
 * stock's letters. */
static int index_face(struct native_list *render, int i)
{
    static const char *const paths[] = {
        FONT_DIR "/56-podbox-index.pbf", FONT_DIR "/30-noto-serif-figures.pbf",
        FONT_DIR "/46-helvetica-bold-index.pbf",
    };
    if (render->index_faces[i] < 0)
        render->index_faces[i] = font_load_ex(paths[i], 0, INDEX_GLYPHS);
    return render->index_faces[i];
}

/* Loads the keys either side of `key` ahead of the wheel, so the next letter
 * shows on the frame it is due. A 56 px face's cache holds about 20 of its
 * letters, not the whole alphabet. */
static void index_near(int font, const char *key, const char *order)
{
    const char *at = key[0] && !key[1] ? strchr(order, key[0]) : NULL;
    if (!at)
        return;
    char near[2 * INDEX_NEAR + 2];
    const char *from = MAX(order, at - INDEX_NEAR);
    strlcpy(near, from, MIN(sizeof(near), (size_t)(at - from) + INDEX_NEAR + 2));
    if (font_prefetch_active())
        font_prefetch_text(font, near);
}

/* Whether stock shows its "123" for `key`: digits, symbols, and a year or
 * track number of the list's own. */
static bool index_digits(const char *key)
{
    return isdigit((unsigned char)key[0]) || !strcmp(key, QUICKSCROLL_DIGITS);
}

/* The key the overlay draws: the new one once its glyphs are ready, until
 * then the one before, so a change of key never shows an empty box. */
static const char *index_text(struct native_list *render, int font, bool ready,
                              const char *order)
{
    const char *key = render->overlay.key;
    if (strcmp(render->index_shown, key) &&
        (ready || list_text_ready(font, key)))
        strlcpy(render->index_shown, key, sizeof(render->index_shown));
    index_near(font, key, order);
    return render->index_shown;
}

/* Stock's overlay: its tile centred across the list and down the screen,
 * and on it the letter in white, or for digits and symbols its "123". */
static void paint_stock_index(struct native_list *render,
                              const struct ui_view_frame *frame)
{
    struct ui_rect tile = {
        frame->bounds.x + (frame->bounds.width - INDEX_TILE_WIDTH) / 2,
        (LCD_HEIGHT - INDEX_TILE_HEIGHT) / 2,
        INDEX_TILE_WIDTH, INDEX_TILE_HEIGHT,
    };
    struct ui_rect clip = intersection(frame->clip, tile);
    if (clip.width <= 0 || clip.height <= 0)
        return;
    int font = index_face(render, 2);
    if (font < 0)
        font = FONT_UI;
    const char *text = index_text(render, font,
                                  index_digits(render->overlay.key),
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (!*text)
        return;
    /* Composed at 8 bits a channel and written once, as stock's 32-bit
     * layer is: what lies below, the tile twice, as stock blends it, then
     * the letter or "123" in white. */
    static uint32_t pixels[INDEX_TILE_WIDTH * INDEX_TILE_HEIGHT];
    struct stock_canvas canvas = { pixels, tile.x, tile.y, tile.width,
                                   tile.height };
    for (int y = clip.y; y < clip.y + clip.height; y++)
        for (int x = clip.x; x < clip.x + clip.width; x++)
        {
            fb_data p = *FBADDR(x, y);
            pixels[(y - tile.y) * tile.width + x - tile.x] =
                (uint32_t)FB_UNPACK_RED(p) << 16 |
                (uint32_t)FB_UNPACK_GREEN(p) << 8 | FB_UNPACK_BLUE(p);
        }
    for (int pass = 0; pass < 2; pass++)
        stock_art_put(&canvas, STOCK_ART_SYSTEM_QUICKSCROLL, tile.x, tile.y);
    if (index_digits(text))
        stock_art_mask(&canvas, STOCK_ART_SYSTEM_QUICKSCROLL_123, tile.x,
                       tile.y, 0xffffff);
    else
    {
        int width = font_getstringsize((const unsigned char *)text, NULL, NULL,
                                       font);
        /* Stock rounds the odd half pixel of centring to the right. */
        stock_canvas_text(&canvas, font, text,
                          tile.x + (tile.width - width + 1) / 2,
                          tile.y + INDEX_TEXT_BASELINE - font_get(font)->ascent,
                          0xffffff);
    }
    stock_canvas_show(&canvas, clip);
}

/* A square twice the letter face's line height, dimmed, with a faint
 * border, the same size for every key. */
static void paint_index(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    if (!render->overlay.key[0])
        return;
    if (stock_index(render))
    {
        paint_stock_index(render, frame);
        return;
    }

    int letters = index_face(render, 0);
    int font = letters >= 0 ? letters : FONT_UI;
    int height = font_get(font)->height;
    int side = 2 * height;
    const char *text = index_text(render, font, false,
                                  "#ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    if (!*text)
        return;
    int width = font_getstringsize((const unsigned char *)text, NULL, NULL, font);
    if (width > side - height / 2 && index_face(render, 1) >= 0)
    {
        font = render->index_faces[1];
        width = font_getstringsize((const unsigned char *)text, NULL, NULL, font);
    }
    struct ui_rect area = {
        frame->bounds.x + (frame->bounds.width - side) / 2,
        frame->bounds.y + (frame->bounds.height - side) / 2,
        side, side,
    };
    struct viewport vp;
    if (set_clip(&vp, frame->clip, area, font, LCD_WHITE))
    {
        unsigned dim = render->colors[UI_COLOR_DIM];
        int right = vp.x + vp.width;
        for (int y = vp.y; y < vp.y + vp.height; y++)
        {
            fb_data *row = FBADDR(vp.x, y);
            ui_blit_tint(row, row, vp.width, INDEX_SCRIM_RETAIN, dim);
            if (y == area.y || y == area.y + area.height - 1)
                ui_blit_tint(row, row, vp.width, INDEX_BORDER_RETAIN, LCD_WHITE);
            else
            {
                if (vp.x == area.x)
                    ui_blit_tint(row, row, 1, INDEX_BORDER_RETAIN, LCD_WHITE);
                if (right == area.x + area.width)
                    ui_blit_tint(row + vp.width - 1, row + vp.width - 1, 1,
                                 INDEX_BORDER_RETAIN, LCD_WHITE);
            }
        }

        struct viewport *prior = lcd_set_viewport(&vp);
        lcd_putsxy(area.x - vp.x + (side - width) / 2,
                   area.y - vp.y + (side - font_get(font)->height) / 2,
                   (const unsigned char *)text);
        lcd_set_viewport(prior);
    }
}

static const struct ui_view_ops bg_ops = { .paint = paint_background };
static const struct ui_view_ops layer_ops = { .paint = ui_layer_paint };
static const struct ui_view_ops title_ops = { .paint = paint_title };
static const struct ui_view_ops album_header_ops = { .paint = paint_album_header };
static const struct ui_view_ops row_ops = { .paint = paint_rows };
static const struct ui_view_ops note_ops = { .paint = paint_notes };
static const struct ui_view_ops index_ops = { .paint = paint_index };

static void paint_pane(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    menu_pane_paint(frame->clip);
}
static const struct ui_view_ops pane_ops = { .paint = paint_pane };

/* The selected row as the pane shows it. */
static void select_pane(struct native_list *render)
{
    struct gui_synclist *list = render->owner;
    struct menu_pane_content content = { .kind = LIST_PANE_INFO, .art = -1 };
    int item = list->selected_item;
    if (item >= 0 && item < list->nb_items)
    {
        content.kind = list->callback_get_item_pane(item, list->data,
                                                    &content.art);
        char *name = render->scratch.row.name;
        const unsigned char *label_ptr = (const unsigned char *)
            list->callback_get_item_name(item, list->data, name,
                                         sizeof(render->scratch.row.name));
        const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER
                            ? (const char *)P2STR(label_ptr) : "";
        strlcpy(content.title, label, sizeof(content.title));
        const char *value = list->callback_get_item_value ?
            list->callback_get_item_value(item, list->data,
                                          render->scratch.row.value,
                                          sizeof(render->scratch.row.value)) :
            NULL;
        if (value)
            strlcpy(content.caption, value, sizeof(content.caption));
    }
    if (content.kind == LIST_PANE_DEVICE || content.kind == LIST_PANE_SHUFFLE)
        strlcpy(content.title, native_chrome_title(str(LANG_ROCKBOX_TITLE)),
                sizeof(content.title));
    menu_pane_select(&content, list->generation);
}

static void add_view(struct native_list *render, struct ui_view *node,
                     struct ui_rect rect, const struct ui_view_ops *ops, void *data)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
    ui_view_init(node, rect, ops, data);
    ui_view_append(&render->root, node);
}

static void refresh_backdrop(struct native_list *render, bool visible)
{
    int mode = render->theme.background[UI_BG_LIST];
    int art = mode == UI_BG_SOLID || !visible ? -1 : album_art_current();
    unsigned generation = album_art_generation();
    unsigned int fg = render->colors[UI_COLOR_FOREGROUND];
    if (art == render->art_handle && generation == render->art_generation &&
        fg == render->art_text_color && mode == render->art_mode)
        return;

    ui_layer_free(&render->backdrop);
    render->art_handle = art;
    render->art_generation = generation;
    render->art_text_color = fg;
    render->art_mode = mode;
    if (art <= 0 || !ui_layer_alloc(&render->backdrop, LCD_WIDTH, LCD_HEIGHT))
        return;
    fb_data *pixels = core_get_data_pinned(render->backdrop.handle);
    bool ok = album_art_render_background(pixels, LCD_WIDTH, LCD_HEIGHT, art,
                 mode == UI_BG_BLURRED_COVER, fg);
    core_put_data_pinned(pixels);
    if (!ok)
        ui_layer_free(&render->backdrop);
}

/* The panel's scan reaches row y this long after the TE rise: the TE
 * pulse, then the rows at an even pace through the rest of the refresh. */
#define TE_PULSE_US 1100
#define SCAN_MARGIN_US 500

static uint32_t scan_reaches(int y)
{
    uint32_t period = ui_frame_period();
    return TE_PULSE_US + (period - TE_PULSE_US) * MAX(0, y) / LCD_HEIGHT +
           SCAN_MARGIN_US;
}

/* A marker frame for the next refresh is drawn once the scan of the
 * refresh showing the last one has reached the marker. The LCD driver then
 * cannot start it ahead of that scan, which would show it a refresh early
 * on top of the last frame, and it has most of a refresh to reach the
 * panel behind the scan. */
static uint32_t snap_deadline(const struct native_list *render)
{
    const struct list_motion *motion = &render->motion;
    struct list_pose next = motion_sample(motion,
        MIN(motion->painted + 1, motion_duration(motion)));
    int y = motion->bounds.y +
            (next.cursor - next.top) * motion->pitch / ROW_UNIT;
    if (render->shown.valid)
        y = MAX(y, render->shown.cursor_y);
    return render->presented + scan_reaches(y);
}

/* Whether the next marker frame is due. A deadline further ahead than any
 * frame queues is left from long ago. */
static bool snap_due(const struct native_list *render, uint32_t now)
{
    int32_t left = snap_deadline(render) - now;
    return left <= 0 || left > 3 * (int32_t)ui_frame_period();
}

/* Records the refresh the marker frame just drawn reaches. */
static void snap_presented(struct native_list *render, bool submitted)
{
    uint32_t now = USEC_TIMER, refresh;
    if (submitted && ui_present_refresh(now, &refresh))
    {
        render->presented = refresh;
        return;
    }
    /* Unreported, the frame counts for the next refresh, and keeps to the
     * refresh grid unless a whole refresh has gone by without one. */
    int32_t period = ui_frame_period();
    uint32_t soonest = now, phase = ui_clock_phase(now);
    if (phase != UINT32_MAX)
        soonest += period - phase;
    uint32_t next = render->presented + period;
    int32_t late = soonest - next;
    render->presented = late >= period || late < -3 * period ? soonest : next;
}

/* While a glide goes by hardware-scroll frames, each is drawn once the
 * last one is on the panel, for the refresh after it, or for the first
 * one it can still reach. */
static uint32_t batch_deadline(const struct native_list *render)
{
    return render->presented +
           scan_reaches(render->theme.metric[UI_METRIC_STATUS_HEIGHT]);
}

static unsigned batch_frame(const struct native_list *render)
{
    uint32_t period = ui_frame_period();
    uint32_t target = render->presented + period;
    int32_t late = (uint32_t)USEC_TIMER + period / 2 - target;
    if (late >= 0)
        target += (late / period + 1) * period;
    return ui_clock_frame(target - render->motion.started + period / 2);
}

static bool update_row_pose(struct native_list *render, const struct list_geometry *g)
{
    struct gui_synclist *list = render->owner;
    struct list_motion *motion = &render->motion;
    struct list_pose pose = { fixed_row(MAX(0, list->start_item[SCREEN_MAIN])),
                              fixed_row(list->selected_item) };
    bool finished = false;
    if (motion_enabled() && motion_matches(motion, g, list))
    {
        if (motion->snap)
        {
            /* A jump of the rows is a frame of its own and carries the
             * marker's next step. */
            bool jump = render->shown.valid && render->shown.top !=
                        MAX(0, list->start_item[SCREEN_MAIN]);
            if (motion->painted < motion_duration(motion) &&
                (jump || snap_due(render, USEC_TIMER)))
            {
                motion->painted++;
                render->snap_frame = true;
            }
        }
        else if (render->batched)
            motion->painted = batch_frame(render);
        else
            motion->painted =
                ui_clock_frame((uint32_t)USEC_TIMER - motion->started);
        pose = motion_sample(motion, motion->painted);
        finished = motion->painted >= motion_duration(motion);
    }
    else if (motion->visible)
    {
        motion_stop(motion);
    }

    int first = pose.top / ROW_UNIT;
    int fraction = pose.top % ROW_UNIT;
    if (fraction < 0)
    {
        --first;
        fraction += ROW_UNIT;
    }
    int spare = row_spare(&render->theme, g->rows.height, g->pitch);
    int64_t cursor = pose.cursor - pose.top;
    render->row_shift = -fraction * g->pitch / ROW_UNIT;
    render->cursor_y = g->rows.y + cursor * g->pitch / ROW_UNIT;
    render->cursor_height = g->pitch * render->owner->selected_size;
    if (spare && cursor >= 0)
    {
        int slot = cursor / ROW_UNIT;
        int top = slot_y(slot, g->pitch, spare);
        int height = slot_y(slot + 1, g->pitch, spare) - top;
        render->cursor_y = g->rows.y + top +
                           (cursor % ROW_UNIT) * height / ROW_UNIT;
        render->cursor_height = slot_y(slot + render->owner->selected_size,
                                       g->pitch, spare) - top;
    }
    render->rows.value[UI_SCROLL_OFFSET] = first;
    render->rows.value[UI_SELECTION] = (pose.cursor + HALF_ROW) / ROW_UNIT;
    return finished;
}

static bool marquee_resting(const struct native_list *render)
{
    uint32_t elapsed = USEC_TIMER - render->marquee_started;
    if (stock_marquee(render))
        return !render->has_marquee ||
               elapsed / STOCK_MARQUEE_TICK < STOCK_MARQUEE_WAIT;
    return !render->has_marquee ||
           ui_clock_frame(elapsed) <
           ui_clock_frame(global_settings.scroll_delay * 1000);
}

/* Whether the rows the last paint left differ from this frame's only by
 * where the list and its selection stand. */
static bool shown_current(const struct native_list *render,
                          const struct list_geometry *g)
{
    const struct list_shown *shown = &render->shown;
    return shown->valid && shown->pitch == g->pitch &&
           !memcmp(&shown->rows, &g->rows, sizeof(g->rows)) &&
           shown->theme == render->theme_generation &&
           shown->palette == render->palette_generation &&
           shown->art == render->art_generation &&
           shown->art_handle == render->art_handle &&
           (shown->font == render->font_epoch || shown->complete) &&
           shown->artist == render->artist_generation &&
           marquee_resting(render);
}

/* Under snapped scrolling, a frame whose rows stand where the last paint
 * left them repaints only the marker's path, plus the rows whose text
 * colour follows it. False when anything else may have changed. */
static bool marker_damage(const struct native_list *render,
                          const struct list_geometry *g, struct ui_rect *damage)
{
    const struct list_shown *shown = &render->shown;
    const struct gui_synclist *list = render->owner;
    int top = render->rows.value[UI_SCROLL_OFFSET];
    int lit = render->rows.value[UI_SELECTION];
    if (!shown_current(render, g) ||
        render->theme.style[UI_STYLE_LIST_SCROLL] != UI_LIST_SCROLL_SNAP ||
        render->row_shift || shown->top != top ||
        (g->notes.height > 0 && shown->selected != list->selected_item))
        return false;

    /* The tallest run of selected_size rows. */
    int spare = row_spare(&render->theme, g->rows.height, g->pitch);
    int height = slot_y(list->selected_size, g->pitch, spare);
    struct ui_rect band = { g->rows.x, MIN(shown->cursor_y, render->cursor_y),
                            g->rows.width, 0 };
    band.height = MAX(shown->cursor_y, render->cursor_y) + height - band.y;
    if (lit != shown->lit)
    {
        struct ui_rect row = { g->rows.x, 0, g->rows.width, height };
        row.y = g->rows.y + slot_y(shown->lit - top, g->pitch, spare);
        band = bounding(band, row);
        row.y = g->rows.y + slot_y(lit - top, g->pitch, spare);
        band = bounding(band, row);
    }
    else if (shown->cursor_y == render->cursor_y)
        band.height = 0;
    else if (shown->marker.width > 0)
    {
        band.x = shown->marker.x;
        band.width = shown->marker.width;
    }
    *damage = intersection(band, g->rows);
    if (damage->width <= 0 || damage->height <= 0)
        *damage = (struct ui_rect){0};
    if (render->indicator != shown->indicator)
    {
        int width = scrollbar_width(list, g->rows, g->rows.height / g->pitch);
        struct ui_rect bar = { g->rows.x + g->rows.width - width, g->rows.y,
                               width, g->rows.height };
        *damage = damage->width > 0 ? bounding(*damage, bar) : bar;
    }
    return true;
}

/* A paint a page transition held back reached the panel with the
 * transition's last frame, which shows the framebuffer whole. */
static void adopt_framebuffer(struct native_list *render)
{
    struct list_shown *shown = &render->shown;
    unsigned int writes;
    if (shown->valid && shown->held &&
        ui_hwscroll_framebuffers(&writes) != shown->framebuffers)
    {
        shown->held = false;
        shown->on_panel = true;
        shown->writes = writes;
    }
}

/* A full-width list over a plain background, whose last paint the panel
 * still shows untouched, can reach it as a hardware-scroll frame. */
static bool batch_ready(const struct native_list *render,
                        const struct list_geometry *g)
{
    const struct gui_synclist *list = render->owner;
    return render->shown.on_panel && shown_current(render, g) &&
           render->backdrop.handle <= 0 && list->default_viewport &&
           !list->selection_color && !list->callback_draw_item &&
           g->background.x == 0 && g->background.width == LCD_WIDTH &&
           g->background.y + g->background.height == LCD_HEIGHT &&
           ui_hwscroll_writes() == render->shown.writes;
}

static void batch_pose(const struct native_list *render,
                       struct list_hwscroll_pose *pose)
{
    const struct list_shown *shown = &render->shown;
    struct ui_rect marker = { shown->marker.x, shown->cursor_y,
                              shown->marker.width, shown->cursor_height };
    if (render->theme.style[UI_STYLE_SELECTION] == UI_SELECTION_BAR)
    {
        marker.x = 0;
        marker.width = LCD_WIDTH;
    }
    *pose = (struct list_hwscroll_pose){
        .top = shown->top_px, .marker = marker, .lit = shown->lit,
        .selected = shown->selected, .indicator = shown->indicator,
    };
}

static struct ui_rect slot_row(const struct native_list *render,
                               const struct list_geometry *g, int slot)
{
    int spare = row_spare(&render->theme, g->rows.height, g->pitch);
    int top = g->rows.y + render->row_shift;
    struct ui_rect row = { g->rows.x, top + slot_y(slot, g->pitch, spare),
                           g->rows.width, 0 };
    row.height = top + slot_y(slot + 1, g->pitch, spare) - row.y;
    return intersection(row, g->rows);
}

static bool rows_waiting(const struct list_shown *shown)
{
    for (int slot = 0; slot < SHOWN_ROWS; ++slot)
        if (shown->skipped[slot])
            return true;
    return false;
}

/* Repaints the rows whose text last waited for glyphs, and only them,
 * until REPAIR_ROWS rows of them have changed. True when rows are left
 * for the next frame. */
static bool repaint_waiting(struct native_list *render,
                            const struct list_geometry *g)
{
    int rows = 0;
    for (int slot = 0; slot < MIN(render->shown.slots, SHOWN_ROWS); ++slot)
    {
        struct ui_rect row = slot_row(render, g, slot);
        if (!render->shown.skipped[slot] || row.height <= 0)
            continue;
        if (rows + row.height > REPAIR_ROWS)
            return true;
        render->page.damage = row;
        ui_page_paint(&render->page);
        if (render->skipping[slot] != render->shown.skipped[slot])
            rows += row.height;
    }
    return false;
}

/* Whether this frame leaves the list as the last paint did. */
static bool list_still(const struct native_list *render,
                       const struct list_geometry *g)
{
    const struct list_shown *shown = &render->shown;
    return shown_current(render, g) && !render->row_shift &&
           shown->top == render->rows.value[UI_SCROLL_OFFSET] &&
           shown->lit == render->rows.value[UI_SELECTION] &&
           shown->cursor_y == render->cursor_y &&
           shown->selected == render->owner->selected_item &&
           shown->indicator == render->indicator;
}

/* The rows the paint just done drew with other text than the last paint
 * left where they stand now: text whose glyphs have loaded since. As
 * LIST_HWSCROLL_REPAIRS runs at most; the last takes any overflow. */
static int repaired_rows(const struct native_list *render,
                         const struct list_geometry *g, struct ui_rect *runs)
{
    const struct list_shown *shown = &render->shown;
    int top = render->rows.value[UI_SCROLL_OFFSET];
    int count = 0;
    for (int slot = 0; shown->valid && slot < SHOWN_ROWS; ++slot)
    {
        int old = top + slot - shown->top;
        struct ui_rect row = slot_row(render, g, slot);
        if (old < 0 || old >= MIN(shown->slots, SHOWN_ROWS) ||
            row.height <= 0 ||
            render->skipping[slot] == shown->skipped[old])
            continue;
        struct ui_rect *last = count ? &runs[count - 1] : NULL;
        if (last && (last->y + last->height == row.y ||
                     count == LIST_HWSCROLL_REPAIRS))
            *last = bounding(*last, row);
        else
            runs[count++] = row;
    }
    return count;
}

/* Sends the frame just painted, and `other`, the damage outside the list,
 * as a hardware-scroll frame, with the `repairs` rows in `repair` resent
 * where they stand. False when they must go by update_rect(). */
static bool batch_submit(struct native_list *render,
                         const struct list_geometry *g,
                         const struct list_hwscroll_pose *before,
                         const struct ui_rect *other,
                         const struct ui_rect *repair, int repairs)
{
    const struct gui_synclist *list = render->owner;
    int visible = g->rows.height / g->pitch;
    int bar = scrollbar_width(list, g->rows, visible);
    /* The planner places rows a pitch apart, so rows that fill the list
     * unevenly may only stand still. */
    int spare = row_spare(&render->theme, g->rows.height, g->pitch);
    int height = visible * g->pitch + spare;
    struct list_hwscroll_layout layout = {
        .width = LCD_WIDTH, .height = LCD_HEIGHT, .area_top = g->background.y,
        .content = { g->rows.x, g->rows.y, g->rows.width, height },
        .scrollbar = { scrollbar_left(render, list) ? g->rows.x :
                       g->rows.x + g->rows.width - bar, g->rows.y, bar,
                       height },
        .notes = g->notes, .pitch = g->pitch, .span = list->selected_size,
    };
    struct list_hwscroll_pose after;
    struct ui_hwscroll_frame frame;
    batch_pose(render, &after);
    after.repair = repair;
    after.repairs = repairs;
    if ((spare && after.top != before->top) ||
        !list_hwscroll_plan(&layout, before, &after, &frame))
        return false;
    render->batched = false;
    if (!frame.shift && !frame.count)
    {
        /* Nothing on the panel changes. */
        ui_take_dirty(SCREEN_MAIN);
        ui_mark_dirty_rect(SCREEN_MAIN, other->x, other->y, other->width,
                           other->height);
        ui_flush_dirty();
        return true;
    }
    if (!ui_hwscroll_submit(&frame, other))
        return false;
    ui_take_dirty(SCREEN_MAIN);
    render->batched = ui_hwscroll_latest(&render->presented);
    return true;
}

static void prefetch_list_page(struct native_list *render, const struct list_geometry *g)
{
    struct gui_synclist *list = render->owner;
    int start = MAX(0, render->rows.value[UI_SCROLL_OFFSET]);
    int rows = g->rows.height / g->pitch + 1;
    int dim = MIN(render->theme.metric[UI_METRIC_LIST_ART_SIZE], g->pitch);
    int font = render->text_font >= 0 ? render->text_font : list->parent[SCREEN_MAIN]->font;
    if (!list->callback_get_item_name || start == render->prefetched_top)
        return;
    render->prefetched_top = start;
    for (int i = start; i < MIN(list->nb_items, start + 2 * rows); i++)
    {
        if (list->callback_prefetch_art)
            list->callback_prefetch_art(i, list->data, dim);
        const unsigned char *label = (const unsigned char *)list->callback_get_item_name(
            i, list->data, render->scratch.row.name, sizeof(render->scratch.row.name));
        if (label && P2ID(label) <= VOICEONLY_DELIMITER)
            font_prefetch_text(font, P2STR(label));
    }
}

/* keep_pane leaves the pane as it is on screen: the list moved, not it.
 * moved says the rows' contents are as the last frame left them. */
static void draw_list(struct gui_synclist *list, bool status_only,
                      bool keep_pane, bool moved)
{
    list_render_init();
    if (renderer_handle <= 0)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    bool same_owner = motion_owner(render, list);
    if (status_only && !same_owner)
        goto done;
    if (list->callback_prefetch_art)
    {
        unsigned generation = artist_tile_generation();
        if (generation != render->artist_generation)
            status_only = false;
        render->artist_generation = generation;
    }
    /* Present complete glyphs in plugin lists and native modals before input. */
    bool prefetch = !rock_ui_native();
    if (prefetch)
        font_prefetch_begin();
    unsigned epoch = font_prefetch_epoch();
    if (render->font_epoch != epoch)
    {
        render->font_epoch = epoch;
        status_only = false;
        /* The pane's text waited for the same glyphs. */
        keep_pane = false;
    }
    if (!same_owner)
    {
        render->shown.valid = false;
        motion_stop(&render->motion);
    }
    bool animate = render->motion.visible;
    if (render->overlay.qs.mode && !index_touched())
        status_only = false;
    if (animate)
    {
        status_only = false;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(true);
#endif
    }

    refresh_tokens(render);
    if (!same_owner || render->selection != list->selected_item)
    {
        render->prefetched_top = -1;
        render->owner = list;
        render->generation = list->generation;
        render->selection = list->selected_item;
        render->marquee_started = ui_clock_start();
        child_reset(render);
    }
    if (!status_only)
    {
        album_palette_refresh();
        refresh_colors(render);
        refresh_backdrop(render, list->default_viewport &&
                         viewportmanager_theme_enabled(SCREEN_MAIN) && !list->selection_color);
    }
    bool show_index = step_index(render);
    bool split = split_list(list);
    if (!status_only)
    {
        menu_pane_page(split);
        if (split)
            select_pane(render);
    }
    struct list_geometry g = measure(list, SCREEN_MAIN, render->theme.metric);
    if (!status_only)
        ui_scene_push_rows(g.rows.y, g.pitch, MIN(g.rows.height / g.pitch,
                           list->nb_items - MAX(0, list->start_item[SCREEN_MAIN])));

    ui_view_init(&render->root, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT}, NULL, NULL);
    /* Put the album layer behind the content and below the status bar. */
    if (render->backdrop.handle > 0)
    {
        ui_view_init(&render->art, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT},
                     &layer_ops, &render->backdrop);
        render->art.value[UI_CLIP_Y] = g.background.y;
        render->art.value[UI_CLIP_HEIGHT] = g.background.height;
        ui_view_append(&render->root, &render->art);
    }
    else
    {
        ui_view_init(&render->art, g.background, &bg_ops, render);
        ui_view_append(&render->root, &render->art);
    }
    add_view(render, &render->title, g.title, &title_ops, render);
    add_view(render, &render->header, g.header, &album_header_ops, render);
    ui_view_init(&render->rows, g.rows, &row_ops, render);
    bool motion_finished = update_row_pose(render, &g);
    if (prefetch && !status_only)
        prefetch_list_page(render, &g);
    ui_view_append(&render->root, &render->rows);
    add_view(render, &render->notes, g.notes, &note_ops, render);
    if (show_index)
        add_view(render, &render->index_view, g.background, &index_ops, render);
    if (split)
        add_view(render, &render->pane, menu_pane_rect(), &pane_ops, render);
    ui_page_init(&render->page, &render->root);
    render->page.focus = &render->rows;
    render->page.damage = split && !keep_pane ?
        (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT} : g.background;
    int bar_width = scrollbar_width(list, g.rows, g.rows.height / g.pitch);
    render->indicator = classic_indicator(render) && bar_width ?
                        indicator_level(render) : 0;
    /* A split menu's pane follows the selection, so only a frame that
     * keeps the pane may shrink to the marker's path. */
    bool partial = moved && !status_only && !show_index &&
                   (!split || keep_pane) &&
                   marker_damage(render, &g, &render->page.damage);
    /* Glyphs have loaded since rows of a list left in place waited. */
    bool repair = moved && !status_only && !show_index &&
                  (!split || keep_pane) &&
                  (render->shown.font != render->font_epoch ||
                   render->repairing) &&
                  rows_waiting(&render->shown) &&
                  (partial || list_still(render, &g));
    if (repair && !partial)
    {
        partial = true;
        render->page.damage = (struct ui_rect){0};
    }
    if (!status_only && !repair)
        render->repairing = false;
    if (status_only)
    {
        int width = render->theme.metric[UI_METRIC_LIST_ICON_SIZE] +
                    render->theme.metric[UI_METRIC_LIST_ICON_MARGIN];
        int left = g.rows.x + (scrollbar_left(render, list) ? bar_width : 0);
        int x = VP_IS_RTL(list->parent[SCREEN_MAIN]) ?
                left + g.rows.width - bar_width - width : left;
        render->page.damage = (struct ui_rect){x, g.rows.y, width, g.rows.height};
    }
    else
    {
        if (!partial)
            render->has_marquee = false;
        render->marquee_frame = ui_clock_frame(USEC_TIMER - render->marquee_started);
        sb_set_title_text(list->title, list->title_icon, SCREEN_MAIN);
    }

    bool held = ui_flush_inhibited();
    adopt_framebuffer(render);
    struct list_hwscroll_pose before;
    bool batch = moved && !status_only && !show_index && !split &&
                 (!partial || repair) &&
                 !held && !gui_synclist_flush_inhibited() &&
                 batch_ready(render, &g);
    if (batch)
        batch_pose(render, &before);
    ui_inhibit_flush(true);
    if (!status_only)
        sb_update(SCREEN_MAIN, false);
    /* The status bar's damage goes ahead of a hardware-scroll frame. */
    struct ui_dirty_rect status = {0};
    if (batch)
        ui_take_dirty_rects(SCREEN_MAIN, &status);
    bool painted = render->page.damage.width > 0;
    if (!status_only && painted)
        render->shown.marker.width = 0;
    /* A partial paint leaves the rest as the last one did. */
    bool complete = !partial || render->shown.complete;
    text_deferred = false;
    render->slots = partial ? render->shown.slots : 0;
    if (partial)
        memcpy(render->skipping, render->shown.skipped,
               sizeof(render->skipping));
    else
        memset(render->skipping, 0, sizeof(render->skipping));
    /* Rows over the budget keep their text as shown, and wait for the
     * next frame. */
    render->holding = batch && !partial;
    render->repair_rows = REPAIR_ROWS;
    struct ui_rect damage = ui_page_paint(&render->page);
    render->holding = false;
    if (repair)
        render->repairing = repaint_waiting(render, &g);
    complete &= !text_deferred;
    if (!gui_synclist_flush_inhibited())
        ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width, damage.height);
    struct ui_rect repairs[LIST_HWSCROLL_REPAIRS];
    int nrepairs = batch || repair ? repaired_rows(render, &g, repairs) : 0;
    for (int i = 0; i < nrepairs && !gui_synclist_flush_inhibited(); ++i)
        ui_mark_dirty_rect(SCREEN_MAIN, repairs[i].x, repairs[i].y,
                           repairs[i].width, repairs[i].height);
    painted |= repair;
    /* A repair that found no glyph to add left the panel as it was. */
    bool kept = repair && damage.width <= 0 && !nrepairs &&
                render->shown.on_panel;
    if (!status_only && painted)
    {
        struct ui_rect marker = render->shown.marker;
        render->shown = (struct list_shown) {
            .rows = g.rows, .marker = marker, .pitch = g.pitch,
            .top = render->rows.value[UI_SCROLL_OFFSET],
            .top_px = render->rows.value[UI_SCROLL_OFFSET] * g.pitch -
                      render->row_shift,
            .cursor_y = render->cursor_y,
            .cursor_height = render->cursor_height,
            .lit = render->rows.value[UI_SELECTION],
            .selected = list->selected_item,
            .theme = render->theme_generation,
            .palette = render->palette_generation,
            .art = render->art_generation, .art_handle = render->art_handle,
            .font = render->font_epoch, .artist = render->artist_generation,
            .indicator = render->indicator, .complete = complete,
            .valid = !show_index && !gui_synclist_flush_inhibited(),
        };
        render->shown.slots = render->slots;
        memcpy(render->shown.skipped, render->skipping,
               sizeof(render->shown.skipped));
    }
    ui_inhibit_flush(held);
    unsigned flushes = ui_flush_count();
    struct ui_rect other = { status.x, status.y, status.w, status.h };
    bool sent = batch && batch_submit(render, &g, &before, &other, repairs,
                                      nrepairs);
    if (!sent)
    {
        ui_mark_dirty_rect(SCREEN_MAIN, other.x, other.y, other.width,
                           other.height);
        if (!gui_synclist_flush_inhibited())
            ui_flush_dirty();
    }
    if (!status_only && painted)
    {
        if (!sent)
            render->batched = false;
        render->shown.on_panel = sent || kept || ui_flush_count() != flushes;
        render->shown.writes = ui_hwscroll_writes();
        render->shown.held = !render->shown.on_panel && lcd_updates_held();
        unsigned int writes;
        render->shown.framebuffers = ui_hwscroll_framebuffers(&writes);
    }
    if (render->snap_frame)
    {
        render->snap_frame = false;
        snap_presented(render, ui_flush_count() != flushes);
    }
    if (motion_finished)
        motion_stop(&render->motion);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (animate)
        cpu_boost(false);
#endif
    if (prefetch)
        font_prefetch_end();

done:
    core_put_data_pinned(render);
}

void list_render_draw(struct gui_synclist *list)
{
    draw_list(list, false, false, false);
}

void list_render_draw_moved(struct gui_synclist *list)
{
    draw_list(list, false, false, true);
}

void gui_synclist_draw_status(struct gui_synclist *list)
{
    if (list->callback_draw_status)
        draw_list(list, true, false, false);
}

bool gui_synclist_moving(const struct gui_synclist *list)
{
    if (renderer_handle <= 0)
        return false;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    bool moving = motion_owner(render, list) && render->motion.visible;
    core_put_data_pinned(render);
    return moving;
}

bool gui_synclist_paint_background(const struct gui_synclist *list,
                                   const struct viewport *region)
{
    if (renderer_handle <= 0)
        return false;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    bool same_owner = motion_owner(render, list);
    if (same_owner)
    {
        struct ui_view_frame frame = {
            .bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT},
            .clip = {region->x, region->y, region->width, region->height},
            .opacity = UI_FULL_OPACITY,
            .brightness = UI_FULL_OPACITY,
        };
        render->art.ops->paint(&render->art, &frame);
    }
    core_put_data_pinned(render);
    return same_owner;
}

void list_render_child_input(const struct gui_synclist *list)
{
    if (renderer_handle <= 0 || !list->callback_child_texts)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    if (render->owner == list)
        child_reset(render);
    core_put_data_pinned(render);
}

void list_render_forget(const struct gui_synclist *list)
{
    if (renderer_handle <= 0)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    if (render->owner == list)
    {
        if (render->owner->callback_child_texts)
            child_reset(render);
        render->owner = NULL;
        render->shown.valid = false;
        render->has_marquee = false;
        motion_stop(&render->motion);
        index_release_fonts(render);
        memset(&render->overlay, 0, sizeof(render->overlay));
        render->art_handle = -1;
        ui_layer_free(&render->backdrop);
    }
    core_put_data_pinned(render);
}

void gui_synclist_scroll_stop(struct gui_synclist *list)
{
    list_render_forget(list);
    FOR_NB_SCREENS(i)
        screens[i].scroll_stop_viewport(list->parent[i]);
}

static int list_deadline(const struct gui_synclist *list, bool poll);

static bool child_due(const struct native_list *render, uint32_t now)
{
    return !font_prefetch_busy() &&
           (int32_t)(now - render->child_after) >= 0;
}

struct child_scan {
    int row_font, title_font;
    unsigned budget;
};

static void child_text(int row, const char *text, void *context)
{
    struct child_scan *scan = context;
    int font = row < 0 ? scan->title_font : scan->row_font;
    if (font >= 0)
        font_prefetch_speculative_text(font, text, &scan->budget);
}

static void child_step(struct native_list *render, struct gui_synclist *list)
{
    int pages = MIN(list->nb_items, CHILD_PAGES);
    int visible = list_render_visible_rows(list, SCREEN_MAIN) + 1;
    int n = render->child_next;
    /* Give the selected page priority, then work around the selection. */
    int item = (list->selected_item + n) % list->nb_items;
    bool chrome = viewportmanager_theme_enabled(SCREEN_MAIN);
    struct child_scan scan = {
        .row_font = render->text_font >= 0 ? render->text_font :
                    list->parent[SCREEN_MAIN]->font,
        .title_font = chrome ? sb_title_font() :
                      render->title_font >= 0 ? render->title_font : FONT_UI,
        .budget = CHILD_GLYPHS - render->child_glyphs,
    };
    unsigned budget = scan.budget;
    /* Sample bodies near the selection, where the next wheel step can
     * reach them. Menu providers use their in-memory labels at any index. */
    int rows = n < visible ? visible : 0;
    font_prefetch_begin();
    bool done = list->callback_child_texts(item, list->data, rows,
                                          child_token, child_text, &scan);
    font_prefetch_end();
    render->child_glyphs += budget - scan.budget;
    if (done)
    {
        __atomic_store_n(&child_token, child_token + 1, __ATOMIC_RELAXED);
        ++render->child_next;
    }
    if (!scan.budget)
        render->child_next = pages;
    /* Give the action queue a tick between children or result polls. */
    render->child_after = USEC_TIMER + (1000000 / HZ);
}

static int child_deadline(const struct native_list *render,
                          const struct gui_synclist *list, bool poll)
{
    if (!list->callback_child_texts || !render->shown.valid ||
        render->child_next >= MIN(list->nb_items, CHILD_PAGES) ||
        render->motion.visible || render->overlay.visible ||
        render->repairing || !render->shown.complete ||
        rows_waiting(&render->shown))
        return TIMEOUT_BLOCK;
    uint32_t now = USEC_TIMER;
    if (child_due(render, now))
        return 0;
    if (!poll)
        return TIMEOUT_BLOCK;
    int32_t left = render->child_after - now;
    return MAX(1, left / (1000000 / HZ) + 1);
}

static int motion_deadline(const struct gui_synclist *list, bool poll)
{
    int delay = list_deadline(list, poll);
    if (renderer_handle <= 0 || ui_scrolling_held())
        return delay;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    bool owner = motion_owner(render, list);
    bool split = owner && split_list(list);
    int child = owner ? child_deadline(render, list, poll) : TIMEOUT_BLOCK;
    core_put_data_pinned(render);
    if (child != TIMEOUT_BLOCK && (delay == TIMEOUT_BLOCK || child < delay))
        delay = child;
    int pane = split ? menu_pane_deadline() : TIMEOUT_BLOCK;
    if (pane != TIMEOUT_BLOCK && (delay == TIMEOUT_BLOCK || pane < delay))
        delay = pane;
    return delay;
}

/* When the list itself next moves, the pane aside. */
static int list_deadline(const struct gui_synclist *list, bool poll)
{
    if (renderer_handle <= 0 || ui_scrolling_held())
        return TIMEOUT_BLOCK;

    struct native_list *render = core_get_data_pinned(renderer_handle);
    int delay = TIMEOUT_BLOCK;
    if (motion_owner(render, list))
    {
        if (render->font_epoch != font_prefetch_epoch() || render->repairing)
            delay = 0;
        else if (poll && font_prefetch_busy())
            delay = 1;
        if (list->callback_prefetch_art &&
            render->artist_generation != artist_tile_generation())
            delay = 0;
        const struct list_motion *motion = &render->motion;
        uint32_t now = USEC_TIMER;
        if (motion->visible)
        {
            uint32_t elapsed = now - motion->started;
            /* A new frame waits for its presentation slot, so a half-rate
             * glide keeps its frame parity. */
            bool due = motion->snap ? snap_due(render, now) :
                       render->batched ?
                       (int32_t)(now - batch_deadline(render)) >= 0 :
                       ui_clock_frame(elapsed) > motion->painted &&
                       (int32_t)(now - glide_pace.due) >= 0;
            if (!motion_enabled() || due)
                delay = 0;
            else if (poll && (motion->snap || render->batched))
            {
                int motion_delay = ui_clock_wait_until(motion->snap ?
                    snap_deadline(render) : batch_deadline(render));
                if (delay == TIMEOUT_BLOCK || motion_delay < delay)
                    delay = motion_delay;
            }
            else if (poll)
            {
                uint32_t deadline = ui_pace_next(&glide_pace, motion->started);
                int motion_delay = ui_clock_wait_until(deadline);
                if (delay == TIMEOUT_BLOCK || motion_delay < delay)
                    delay = motion_delay;
            }
        }
        if (global_settings.scrolling_enabled && render->has_marquee)
        {
            unsigned frames = stock_marquee(render) ?
                MAX(1, ui_clock_frame(STOCK_MARQUEE_TICK)) : marquee_frames();
            unsigned frame = ui_clock_frame(now - render->marquee_started);
            int tick = 1000000 / HZ;
            int ticks = frame / frames != render->marquee_frame / frames ?
                        0 : (ui_frame_clock(frames) + tick - 1) / tick;
            if (delay == TIMEOUT_BLOCK || ticks < delay)
                delay = ticks;
        }
        if (render->shown.valid && render->shown.indicator > 0)
        {
            uint32_t elapsed = now - render->marquee_started;
            uint32_t next = INDICATOR_HOLD + INDICATOR_STEP_US;
            if (elapsed >= INDICATOR_HOLD)
                next += (elapsed - INDICATOR_HOLD) / INDICATOR_STEP_US *
                        INDICATOR_STEP_US;
            int tick = 1000000 / HZ;
            int ticks = indicator_level(render) != render->shown.indicator ?
                        0 : (int)((next - elapsed + tick - 1) / tick);
            if (delay == TIMEOUT_BLOCK || ticks < delay)
                delay = ticks;
        }
        if (render->overlay.qs.mode)
        {
            int ticks = !index_touched() ? 0 : poll ? 1 : TIMEOUT_BLOCK;
            if (ticks != TIMEOUT_BLOCK && (delay == TIMEOUT_BLOCK || ticks < delay))
                delay = ticks;
        }
    }
    core_put_data_pinned(render);
    return delay;
}

bool list_render_motion_due(const struct gui_synclist *list)
{
    return motion_deadline(list, false) == 0;
}

void list_render_draw_due(struct gui_synclist *list)
{
    if (list_deadline(list, false) == 0)
        draw_list(list, false, true, true);
    if (renderer_handle > 0 && !ui_scrolling_held())
    {
        struct native_list *render = core_get_data_pinned(renderer_handle);
        if (motion_owner(render, list) &&
            child_deadline(render, list, false) == 0 &&
            button_queue_empty() && !gui_synclist_flush_inhibited())
            child_step(render, list);
        core_put_data_pinned(render);
    }
    struct ui_rect damage = menu_pane_step();
    if (damage.width > 0 && damage.height > 0 &&
        !gui_synclist_flush_inhibited())
    {
        ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width,
                           damage.height);
        ui_flush_dirty();
    }
}

int list_render_timeout(const struct gui_synclist *list, int timeout)
{
    int delay = motion_deadline(list, true);
    if (delay != TIMEOUT_BLOCK && (timeout == TIMEOUT_BLOCK || timeout > delay))
        return delay;
    return timeout;
}
