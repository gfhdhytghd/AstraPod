/* Native retained list renderer. Adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include <ctype.h>
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "rbunicode.h"
#include "scroll_engine.h"
#include "settings/settings.h"
#include "system.h"
#include "audio.h"
#include "metadata.h"
#include "draw/bmp.h"
#include "draw/color.h"
#include "draw/round_rect.h"
#include "draw/scrollbar.h"
#include "draw/text_wrap.h"
#include "draw/viewport.h"
#include "metadata/album_palette.h"
#include "system/activity.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/perf.h"
#include "ui/retained.h"
#include "ui/statusbar.h"
#include "list_render.h"
#include "ui/scene.h"
#include "list_select.h"

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
    INDEX_MIN_ITEMS = 51,
    INDEX_WINDOW_F = 10,
    INDEX_TRIGGER = 8,
    INDEX_HOLD_F = 20,
    INDEX_BOX = 96,
    INDEX_INK_WIDTH = INDEX_BOX - 20,
    INDEX_CAP_HEIGHT = 56,
    INDEX_SAMPLES = 12,
    INDEX_SCRIM_RETAIN = 56,  /* 78% dim */
    INDEX_BORDER_RETAIN = 225, /* 12% white */
};

struct list_pose {
    int64_t top, cursor;
};

struct list_motion {
    struct list_pose from, live, rest;
    uint32_t started;
    unsigned painted, trace;
    unsigned glide_frames, spring_frames;
    int visible, count, span;
    struct ui_rect bounds;
    int pitch;
    bool wheel;
};

struct list_index {
    char key[16];
    uint32_t deadline, samples_at[INDEX_SAMPLES];
    int selections[INDEX_SAMPLES];
    int samples, last_sel;
    bool visible, fast;
};

struct index_glyph {
    char key[16];
    int width, height;
    unsigned char mask[(INDEX_INK_WIDTH * INDEX_CAP_HEIGHT + 1) / 2];
};

struct native_list {
    struct ui_theme theme;
    unsigned int colors[UI_COLOR_COUNT];
    unsigned theme_generation, palette_generation;
    bool colors_current;
    int text_font, title_font, icon_font;
    struct ui_view root, title, header, rows, notes, art, index_view;
    struct ui_page page;
    struct ui_layer backdrop;
    unsigned art_generation;
    unsigned int art_text_color;
    int art_handle;
    int art_mode;
    struct gui_synclist *owner;
    unsigned generation;
    int selection;
    struct list_motion motion;
    struct list_index overlay;
    int index_fonts[4];
    bool index_fonts_loaded;
    unsigned index_slot;
    struct index_glyph index_cache[4];
    int row_shift, cursor_y;
    uint32_t marquee_started, marquee_frame;
    bool has_marquee;
    unsigned char mark_mask[2];
};

static int renderer_handle;

void list_render_init(void)
{
    if (renderer_handle > 0)
        return;
    renderer_handle = core_alloc_free_space(sizeof(struct native_list));
    if (renderer_handle > 0)
    {
        struct native_list *render = core_get_data_pinned(renderer_handle);
        memset(render, 0, sizeof(*render));
        render->overlay.last_sel = -1;
        render->text_font = render->title_font = render->icon_font = -1;
        render->theme_generation = (unsigned)-1;
        core_put_data_pinned(render);
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
    for (unsigned i = 0; i < ARRAYLEN(render->index_cache); ++i)
        render->index_cache[i].key[0] = '\0';
    font_unload(render->text_font);
    font_unload(render->title_font);
    font_unload(render->icon_font);
    int glyphs = global_settings.glyphs_to_cache;
    render->text_font = ui_theme_font_load(&render->theme, UI_FONT_LIST, glyphs);
    render->title_font = ui_theme_font_load(&render->theme, UI_FONT_TITLE, glyphs);
    render->icon_font = ui_theme_font_load(&render->theme, UI_FONT_ICON, glyphs);
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

static struct ui_rect take_top(struct ui_rect *rect, int height)
{
    height = MIN(rect->height, MAX(0, height));
    struct ui_rect top = { rect->x, rect->y, rect->width, height };
    rect->y += height;
    rect->height -= height;
    return top;
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
    return !ui_scrolling_held() && ui_theme_duration(GLIDE_F) != 0;
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

static struct list_pose motion_sample(const struct list_motion *motion, unsigned frame)
{
    struct list_pose pose = motion->live;
    unsigned glide = motion->glide_frames;
    if (frame < glide)
    {
        pose.top = ease_out(motion->from.top, pose.top, frame, glide);
        pose.cursor = ease_out(motion->from.cursor, pose.cursor, frame, glide);
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
    unsigned glide = ui_theme_duration(GLIDE_F);
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
    if (continuing)
    {
        from = motion_sample(motion, frame);
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

    if (physical_end)
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

    unsigned trace = motion->trace;
    if (!trace)
        trace = ui_perf_begin("list-glide");
    *motion = (struct list_motion) {
        .from = from, .live = live, .rest = rest,
        .started = now, .painted = 0, .trace = trace,
        .glide_frames = glide, .spring_frames = ui_theme_duration(SPRING_F),
        .visible = visible, .count = list->nb_items, .span = span,
        .bounds = g.rows, .pitch = g.pitch, .wheel = wheel,
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

static unsigned marquee_frames(void)
{
    return MAX(1, (lcd_scroll_info.ticks * UI_ANIM_FPS + HZ / 2) / HZ);
}

static unsigned marquee_shift(struct native_list *render, unsigned max_offset)
{
    if (!global_settings.scrolling_enabled || ui_scrolling_held() || !max_offset)
        return 0;
    render->has_marquee = true;
    unsigned elapsed = ui_clock_frame(USEC_TIMER - render->marquee_started);
    /* The stored delay setting is expressed in milliseconds. */
    unsigned delay = global_settings.scroll_delay * UI_ANIM_FPS / 1000;
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

static void draw_text(struct native_list *render, struct ui_rect clip,
                      struct ui_rect cell, int font, unsigned int fg,
                      const char *text, bool scroll, int offset, bool centered)
{
    struct viewport vp;
    if (!text || !*text || !set_clip(&vp, clip, cell, font, fg))
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
    if (!offset && scroll)
        offset = marquee_shift(render, overflow);
    struct viewport *prior = lcd_set_viewport(&vp);
    lcd_putsxy(x - offset, cell.y - vp.y, (const unsigned char *)text);
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

static const char *row_icon(enum themable_icons icon, enum list_row_kind kind,
                            const char *label, bool file_view,
                            enum current_activity activity)
{
    if (file_view)
    {
        if (equals_lang(label, LANG_SEARCH)) return "®";
        if (equals_lang(label, LANG_SORT_TYPE)) return "ª";
        if (equals_lang(label, LANG_MOODS)) return "í";
        if (equals_lang(label, LANG_JOURNEYS)) return "î";

        switch (icon)
        {
            case Icon_Audio: return "¡";
            case Icon_Playlist: return "¥";
            case Icon_Folder: return "m";
            case Icon_Font:
            case Icon_Document: return "¦";
            case Icon_Image: return "§";
            case Icon_Book: return "Ę";
            case Icon_Lyrics: return "ė";
            default: return icon == Icon_NOICON ? NULL : "¨";
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
        { LANG_IMAGES, "x" }, { LANG_FEATURED_ARTISTS, "é" },
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
        case Icon_file_view_menu:
        case Icon_Submenu: return "Þ";
        default: return icon == Icon_NOICON ? NULL : "¨";
    }
}

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
           kind == LIST_ROW_CUSTOM_MENU ? "Þ" : NULL;
}

static bool file_rows(enum current_activity activity)
{
    return activity == ACTIVITY_FILEBROWSER || activity == ACTIVITY_PLAYLISTBROWSER ||
           activity == ACTIVITY_DOCUMENTBROWSER || activity == ACTIVITY_IMAGEBROWSER ||
           activity == ACTIVITY_PLUGINBROWSER || activity == ACTIVITY_FILE_SEARCH ||
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

static void paint_row(struct native_list *render, struct ui_rect clip,
                      struct ui_rect row, int item, bool selected,
                      bool marker_only)
{
    struct gui_synclist *list = render->owner;
    const uint16_t *m = render->theme.metric;
    enum current_activity activity = get_current_activity();
    enum list_row_kind kind = list->callback_get_item_kind ?
        list->callback_get_item_kind(item, list->data) : LIST_ROW_PLAIN;
    enum themable_icons icon = list->callback_get_item_icon ?
        list->callback_get_item_icon(item, list->data) : Icon_NOICON;

    bool files = file_rows(activity);
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
        library_menu || books_menu || agents || activity == ACTIVITY_MAINMENU ||
        (database && (kind == LIST_ROW_CUSTOM || kind == LIST_ROW_CUSTOM_MENU ||
         equals_lang(list->title, LANG_CUSTOM_MENU))));
    if (!icon_column && list->show_icons && database && kind == LIST_ROW_COMMAND)
    {
        char name[MAX_PATH];
        const unsigned char *label_ptr = (const unsigned char *)
            list->callback_get_item_name(item, list->data, name, sizeof(name));
        const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER
                            ? P2STR(label_ptr) : "";
        icon_column = !equals_lang(label, LANG_TAGNAVI_ALL_TRACKS) &&
                      !equals_lang(label, LANG_TAGNAVI_ALL_TRACKS_SORTED_BY_ALBUM);
    }
    int marked = list_select_row_state(list, item);
    if (!icon_column && !art && list->show_icons &&
        (kind == LIST_ROW_CONTAINER || kind == LIST_ROW_CUSTOM_MENU))
        icon_column = icon != Icon_NOICON;

    if (marker_only)
    {
        if (art)
            text_x += MIN(m[UI_METRIC_LIST_ART_SIZE], row.height) +
                      m[UI_METRIC_LIST_AUX_PADDING] + 1;
        else if (icon_column || marked >= 0)
            text_x += icon_size + m[UI_METRIC_LIST_ICON_GAP];
        int x = text_x - m[UI_METRIC_LIST_TEXT_PADDING] + margin;
        marker(render, clip, row_cell(row, x, 0, m[UI_METRIC_LIST_SELECTION_WIDTH],
                                      row.height, VP_IS_RTL(list->parent[SCREEN_MAIN])),
               render->colors[UI_COLOR_SELECTION]);
        return;
    }

    char name[MAX_PATH], value_buf[96], now_playing[MAX_PATH];
    const unsigned char *label_ptr = (const unsigned char *)list->callback_get_item_name(
        item, list->data, name, sizeof(name));
    const char *label = label_ptr && P2ID(label_ptr) <= VOICEONLY_DELIMITER ?
                        (const char *)P2STR(label_ptr) : "";
    const char *value = list->callback_get_item_value ?
                        list->callback_get_item_value(item, list->data, value_buf,
                                                      sizeof(value_buf)) : NULL;
    const char *aux = activity == ACTIVITY_MAINMENU ? NULL :
                      row_accessory(list, item, kind, label, value);

    unsigned int fg = render->colors[selected ? UI_COLOR_SELECTED_TEXT : UI_COLOR_MUTED];
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
        draw_owner_row(list, clip, row, item, icon, selected, text_font, fg, bg, label);
        return;
    }

    bool now_playing_row = activity == ACTIVITY_MAINMENU &&
                           equals_lang(label, LANG_NOW_PLAYING);
    if (now_playing_row)
    {
        const struct mp3entry *id3 = audio_current_track();
        if (id3 && id3->title && *id3->title)
        {
            snprintf(now_playing, sizeof(now_playing), "%s: %s", label, id3->title);
            label = now_playing;
        }
    }

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
        draw_bitmap(clip, tile, cover);
        if (!cover && render->icon_font >= 0)
            draw_text(render, clip, icon_cell, render->icon_font, fg,
                      row_icon(icon, kind, label, files, activity), false, 0, false);
        text_x += cover_size + m[UI_METRIC_LIST_AUX_PADDING] + 1;
    }
    else if (icon_column || marked >= 0)
    {
        text_x += icon_size + m[UI_METRIC_LIST_ICON_GAP];
        if (render->icon_font >= 0)
        {
            const char *glyph = marked >= 0 ? (marked ? "×" : "Ö") :
                                row_icon(icon, kind, label, files, activity);
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
    int text_right = aux ? arrow - m[UI_METRIC_LIST_ACCESSORY_GAP] : right;
    if (agents && value && value[0] == 'r' && value[1] && value[2])
    {
        int value_width = 4 * text_height;
        text_right -= value_width + m[UI_METRIC_LIST_AUX_PADDING];
        draw_text(render, clip, row_cell(row, right - value_width, text_y,
                  value_width, text_height, rtl), text_font, fg, value + 2, false, 0, false);
    }
    draw_text(render, clip, row_cell(row, text_x, text_y,
              text_right - text_x, text_height, rtl), text_font, fg, label,
              selected || list->scroll_all, list->offset_position[SCREEN_MAIN], false);
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

static void paint_rows(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    struct gui_synclist *list = render->owner;
    int pitch = row_pitch(list, SCREEN_MAIN, render->theme.metric);
    int visible = frame->bounds.height / pitch;
    struct ui_rect content = frame->bounds;
    content.height = visible * pitch;
    struct ui_rect clip = intersection(frame->clip, content);
    if (clip.width <= 0 || clip.height <= 0)
        return;

    int first = MAX(frame->scroll_offset, 0);
    int end = MIN(frame->scroll_offset + visible + (render->row_shift != 0),
                  list->nb_items);
    struct ui_rect row = content;
    row.y += render->row_shift + (first - frame->scroll_offset) * pitch;
    row.height = pitch;
    int bar_width = scrollbar_width(list, frame->bounds, visible);
    if (list->scrollbar == SCROLLBAR_LEFT)
        row.x += bar_width;
    row.width -= bar_width;

    if (list->selection_color && list->nb_items)
    {
        struct ui_rect highlight = {row.x, render->cursor_y, row.width,
                                    pitch * list->selected_size};
        fill(clip, highlight, list->selection_color->line_color);
    }

    for (int item = first; item < end; ++item, row.y += pitch)
    {
        bool selected = item >= frame->selection && item - frame->selection < list->selected_size;
        if (intersection(row, clip).height > 0)
            paint_row(render, clip, row, item, selected, false);
    }

    /* The bar can be half visible even if its corresponding text is clipped. */
    if (!list->selection_color && !list->callback_draw_item)
    {
        for (int line = 0; line < list->selected_size; ++line)
        {
            int item = frame->selection + line;
            if (item < 0 || item >= list->nb_items)
                break;
            row.y = render->cursor_y + line * pitch;
            if (intersection(row, clip).height > 0)
                paint_row(render, clip, row, item, true, true);
        }
    }

    if (bar_width)
    {
        int bar_x = list->scrollbar == SCROLLBAR_LEFT ? frame->bounds.x :
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
            struct viewport *prior = lcd_set_viewport(&vp);
            gui_scrollbar_draw(&screens[SCREEN_MAIN], bar.x - vp.x, bar.y - vp.y,
                              bar.width, bar.height, list->nb_items * units, start,
                              start + visible * units, VERTICAL | FOREGROUND);
            lcd_set_viewport(prior);
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
    char buffer[2 * MAX_PATH];
    const char *text = list->callback_get_item_description(list->selected_item,
                          list->data, buffer, sizeof(buffer));
    if (!text || !*text)
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

static void index_from_text(const char *text, char *out, size_t n)
{
    ucschar_t ch = 0;

    out[0] = '\0';
    if (!text || n < 2)
        return;
    while (*text == ' ')
        text++;
    utf8decode((const unsigned char *)text, &ch);
    if (!ch)
        return;
    if (ch < 128)
    {
        if (islower((int)ch))
            ch = (ucschar_t)toupper((int)ch);
        out[0] = (ch >= 'A' && ch <= 'Z') ? (char)ch : '#';
        out[1] = '\0';
        return;
    }
    *utf8encode(ch, (unsigned char *)out) = '\0';
}

static void index_key(struct native_list *render, char *out, size_t n)
{
    struct gui_synclist *list = render->owner;
    char name[MAX_PATH];
    const unsigned char *label;
    const char *text;

    out[0] = '\0';
    if (!list || list->selected_item < 0 || list->selected_item >= list->nb_items)
        return;
    if (list->callback_get_index_key)
    {
        const char *key = list->callback_get_index_key(list->selected_item,
                              list->data, name, sizeof(name));
        if (key && *key)
        {
            snprintf(out, n, "%s", key);
            return;
        }
    }
    if (!list->callback_get_item_name)
        return;
    label = (const unsigned char *)list->callback_get_item_name(
        list->selected_item, list->data, name, sizeof(name));
    text = label && P2ID(label) <= VOICEONLY_DELIMITER ?
           (const char *)P2STR(label) : "";
    index_from_text(text, out, n);
}

static void index_sample(struct list_index *index, int selection, uint32_t now)
{
    if (index->samples == INDEX_SAMPLES)
    {
        memmove(index->samples_at, index->samples_at + 1,
                (INDEX_SAMPLES - 1) * sizeof(index->samples_at[0]));
        memmove(index->selections, index->selections + 1,
                (INDEX_SAMPLES - 1) * sizeof(index->selections[0]));
        index->samples--;
    }
    index->samples_at[index->samples] = now;
    index->selections[index->samples++] = selection;
}

static bool step_index(struct native_list *render)
{
    struct gui_synclist *list = render->owner;
    struct list_index *index = &render->overlay;
    uint32_t now = USEC_TIMER;
    uint32_t window = ui_frame_clock(INDEX_WINDOW_F);

    if (!list || list->nb_items < INDEX_MIN_ITEMS)
    {
        memset(index, 0, sizeof(*index));
        index->last_sel = -1;
        return false;
    }

    int selection = list->selected_item;
    bool changed = selection != index->last_sel;
    bool was_visible = index->visible;
    /* A selection that just changed was still on screen at this sample. */
    if (changed && index->last_sel >= 0)
        index_sample(index, index->last_sel, now);
    index->last_sel = selection;

    for (int i = index->samples - 1; i >= 0; i--)
    {
        if (now - index->samples_at[i] >= window)
            break;
        if (abs(selection - index->selections[i]) >= INDEX_TRIGGER)
        {
            index->deadline = index->samples_at[i] + window;
            index->fast = index->visible = true;
            goto update_key;
        }
    }
    if (index->fast)
    {
        if ((int32_t)(now - index->deadline) < 0)
            index->deadline = now;
        index->deadline += ui_frame_clock(INDEX_HOLD_F);
        index->fast = false;
    }
    if ((int32_t)(now - index->deadline) >= 0)
        index->visible = false;

update_key:
    if (index->visible && (changed || !was_visible))
        index_key(render, index->key, sizeof(index->key));
    return index->visible;
}

/* The Rockbox alpha bitmap packs inverted coverage low nibble first.
 * Monochrome fonts use columns of eight vertical pixels. */
static int index_coverage(const struct font *font, const unsigned char *bits,
                          int width, int x, int y)
{
    if (font->depth)
    {
        int pixel = y * width + x;
        return 15 - ((bits[pixel / 2] >> (4 * (pixel & 1))) & 15);
    }
    return bits[(y / 8) * width + x] & (1u << (y & 7)) ? 15 : 0;
}

static bool index_has_char(struct font *font, ucschar_t ch)
{
    if (ch < font->firstchar || ch - font->firstchar >= (unsigned)font->size)
        return false;
    int width = font_get_width(font, ch);
    if (ch == (ucschar_t)font->defaultchar || width != font_get_width(font, font->defaultchar))
        return true;

    /* Large Unicode fonts can store their default glyph for holes. */
    size_t bytes = font->depth ? (width * font->height + 1) / 2 :
                   width * ((font->height + 7) / 8);
    const unsigned char *fallback = font_get_bits(font, font->defaultchar);
    const unsigned char *bits = font_get_bits(font, ch);
    return memcmp(bits, fallback, bytes) != 0;
}

static int index_font(struct native_list *render, const ucschar_t *chars, int count)
{
    int fonts[] = {render->title_font, render->text_font, FONT_UI,
                   render->index_fonts[0], render->index_fonts[1],
                   render->index_fonts[2], render->index_fonts[3]};
    int best = -1;
    unsigned height = 0;
    for (unsigned i = 0; i < ARRAYLEN(fonts); i++)
    {
        int id = fonts[i];
        if (id < 0)
            continue;
        font_lock(id, true);
        struct font *font = font_get(id);
        if (font->height > height)
        {
            int n = 0;
            while (n < count && index_has_char(font, chars[n]))
                n++;
            if (n == count)
            {
                height = font->height;
                best = id;
            }
        }
        font_lock(id, false);
    }
    return best;
}

static void scale_index_glyph(struct index_glyph *glyph, const unsigned char *ink,
                              int stride, int width, int height)
{
    glyph->height = MAX(1, MIN(INDEX_CAP_HEIGHT, INDEX_INK_WIDTH * height / width));
    glyph->width = MAX(1, MIN(INDEX_INK_WIDTH,
                       (glyph->height * width + height / 2) / height));
    memset(glyph->mask, 0, sizeof(glyph->mask));
    /* Map pixel centres in 8.8 fixed point and bilinearly sample coverage. */
    for (int y = 0; y < glyph->height; y++)
    {
        int sy = (2 * y + 1) * height * 128 / glyph->height - 128;
        sy = MAX(0, MIN(sy, (height - 1) * 256));
        const unsigned char *top = ink + (sy / 256) * stride;
        const unsigned char *bottom = ink + MIN(sy / 256 + 1, height - 1) * stride;
        int fy = sy % 256;
        for (int x = 0; x < glyph->width; x++)
        {
            int sx = (2 * x + 1) * width * 128 / glyph->width - 128;
            sx = MAX(0, MIN(sx, (width - 1) * 256));
            int left = sx / 256, right = MIN(left + 1, width - 1);
            int fx = sx % 256;
            int a = top[left] * (256 - fx) + top[right] * fx;
            int b = bottom[left] * (256 - fx) + bottom[right] * fx;
            int alpha = 15 - ((a * (256 - fy) + b * fy + 32768) >> 16);
            int pixel = y * glyph->width + x;
            glyph->mask[pixel / 2] |= alpha << (4 * (pixel & 1));
        }
    }
}

static void raster_index_key(struct native_list *render, struct index_glyph *glyph)
{
    ucschar_t chars[sizeof(glyph->key) - 1];
    const unsigned char *text = (const unsigned char *)glyph->key;
    int count = 0;
    while (*text && count < (int)ARRAYLEN(chars))
        text = utf8decode(text, &chars[count++]);
    int id = index_font(render, chars, count);
    if (id < 0)
        id = render->title_font >= 0 ? render->title_font : FONT_UI;

    font_lock(id, true);
    struct font *font = font_get(id);
    int height = font->height, width = 0;
    for (int i = 0; i < count; i++)
        width += font_get_width(font, chars[i]);

    int handle = width && height ? core_alloc_free_space((size_t)width * height) : -1;
    if (handle > 0)
    {
        unsigned char *ink = core_get_data_pinned(handle);
        int left = width, top = height, right = 0, bottom = 0;
        int start = 0;
        for (int i = 0; i < count; i++)
        {
            int w = font_get_width(font, chars[i]);
            const unsigned char *bits = font_get_bits(font, chars[i]);
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int coverage = index_coverage(font, bits, w, x, y);
                    ink[y * width + start + x] = coverage;
                    if (coverage)
                    {
                        left = MIN(left, start + x);
                        right = MAX(right, start + x + 1);
                        top = MIN(top, y);
                        bottom = MAX(bottom, y + 1);
                    }
                }
            }
            start += w;
        }
        if (right > left)
            scale_index_glyph(glyph, ink + top * width + left, width,
                              right - left, bottom - top);
        core_put_data_pinned(ink);
        core_free(handle);
    }
    font_lock(id, false);
}

static const struct index_glyph *index_glyph(struct native_list *render)
{
    const char *key = render->overlay.key;
    for (unsigned i = 0; i < ARRAYLEN(render->index_cache); i++)
        if (strcmp(render->index_cache[i].key, key) == 0)
            return &render->index_cache[i];

    if (!render->index_fonts_loaded)
    {
        /* Largest bundled text faces, including the dedicated figures face. */
        static const char * const paths[] = {
            FONT_DIR "/30-noto-serif-figures.fnt",
            FONT_DIR "/28-noto-serif.fnt",
            FONT_DIR "/26-noto-sans-medium.fnt",
            FONT_DIR "/24-noto-serif.fnt",
        };
        for (unsigned i = 0; i < ARRAYLEN(paths); i++)
            render->index_fonts[i] = font_load_ex(paths[i], 0, 8);
        render->index_fonts_loaded = true;
    }

    struct index_glyph *glyph = &render->index_cache[render->index_slot];
    render->index_slot = (render->index_slot + 1) % ARRAYLEN(render->index_cache);
    snprintf(glyph->key, sizeof(glyph->key), "%s", key);
    glyph->width = glyph->height = 0;
    raster_index_key(render, glyph);
    return glyph;
}

static void paint_index(const struct ui_view *view, const struct ui_view_frame *frame)
{
    struct native_list *render = view->data;
    if (!render->overlay.key[0])
        return;

    struct ui_rect area = frame->bounds;
    area.x += (area.width - INDEX_BOX) / 2;
    area.y += (area.height - INDEX_BOX) / 2;
    area.width = area.height = INDEX_BOX;
    struct viewport vp;
    if (!set_clip(&vp, frame->clip, area, FONT_UI, LCD_WHITE))
        return;
    const struct index_glyph *glyph = index_glyph(render);
    unsigned dim = render->colors[UI_COLOR_DIM];
    int right = vp.x + vp.width;
    for (int y = vp.y; y < vp.y + vp.height; y++)
    {
        fb_data *row = FBADDR(vp.x, y);
        ui_blit_tint(row, row, vp.width, INDEX_SCRIM_RETAIN, dim);
        if (y == area.y || y == area.y + INDEX_BOX - 1)
            ui_blit_tint(row, row, vp.width, INDEX_BORDER_RETAIN, LCD_WHITE);
        else
        {
            if (vp.x == area.x)
                ui_blit_tint(row, row, 1, INDEX_BORDER_RETAIN, LCD_WHITE);
            if (right == area.x + INDEX_BOX)
                ui_blit_tint(row + vp.width - 1, row + vp.width - 1, 1, INDEX_BORDER_RETAIN, LCD_WHITE);
        }
    }
    if (glyph->width > 0)
    {
        struct viewport *prior = lcd_set_viewport(&vp);
        lcd_alpha_bitmap_part(glyph->mask, 0, 0, glyph->width,
                              area.x - vp.x + (INDEX_BOX - glyph->width) / 2,
                              area.y - vp.y + (INDEX_BOX - glyph->height) / 2,
                              glyph->width, glyph->height);
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

static bool update_row_pose(struct native_list *render, const struct list_geometry *g)
{
    struct gui_synclist *list = render->owner;
    struct list_motion *motion = &render->motion;
    struct list_pose pose = { fixed_row(MAX(0, list->start_item[SCREEN_MAIN])),
                              fixed_row(list->selected_item) };
    bool finished = false;
    if (motion_enabled() && motion_matches(motion, g, list))
    {
        motion->painted = ui_clock_frame((uint32_t)USEC_TIMER - motion->started);
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
    render->row_shift = -fraction * g->pitch / ROW_UNIT;
    render->cursor_y = g->rows.y + (pose.cursor - pose.top) * g->pitch / ROW_UNIT;
    render->rows.value[UI_SCROLL_OFFSET] = first;
    render->rows.value[UI_SELECTION] = (pose.cursor + HALF_ROW) / ROW_UNIT;
    return finished;
}

static void draw_list(struct gui_synclist *list, bool status_only)
{
    list_render_init();
    if (renderer_handle <= 0)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    bool same_owner = motion_owner(render, list);
    if (status_only && !same_owner)
        goto done;
    if (!same_owner)
    {
        motion_stop(&render->motion);
        memset(&render->overlay, 0, sizeof(render->overlay));
        render->overlay.last_sel = -1;
    }
    if (render->motion.visible || render->overlay.visible)
        status_only = false;

    refresh_tokens(render);
    if (render->owner != list || render->generation != list->generation ||
        render->selection != list->selected_item)
    {
        render->owner = list;
        render->generation = list->generation;
        render->selection = list->selected_item;
        render->marquee_started = USEC_TIMER;
    }
    if (!status_only)
    {
        album_palette_refresh();
        refresh_colors(render);
        refresh_backdrop(render, list->default_viewport &&
                         viewportmanager_theme_enabled(SCREEN_MAIN) && !list->selection_color);
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
    ui_view_append(&render->root, &render->rows);
    add_view(render, &render->notes, g.notes, &note_ops, render);
    if (step_index(render))
        add_view(render, &render->index_view, g.background, &index_ops, render);
    ui_page_init(&render->page, &render->root);
    render->page.focus = &render->rows;
    render->page.damage = g.background;
    if (status_only)
    {
        int width = render->theme.metric[UI_METRIC_LIST_ICON_SIZE] +
                    render->theme.metric[UI_METRIC_LIST_ICON_MARGIN];
        int bar_width = scrollbar_width(list, g.rows, g.rows.height / g.pitch);
        int left = g.rows.x + (list->scrollbar == SCROLLBAR_LEFT ? bar_width : 0);
        int x = VP_IS_RTL(list->parent[SCREEN_MAIN]) ?
                left + g.rows.width - bar_width - width : left;
        render->page.damage = (struct ui_rect){x, g.rows.y, width, g.rows.height};
    }
    else
    {
        render->has_marquee = false;
        render->marquee_frame = ui_clock_frame(USEC_TIMER - render->marquee_started);
        sb_set_title_text(list->title, list->title_icon, SCREEN_MAIN);
    }

    bool held = ui_flush_inhibited();
    ui_inhibit_flush(true);
    if (!status_only)
        sb_update(SCREEN_MAIN, false);
    struct ui_rect damage = ui_page_paint(&render->page);
    if (!gui_synclist_flush_inhibited())
        ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width, damage.height);
    ui_inhibit_flush(held);
    if (!gui_synclist_flush_inhibited())
        ui_flush_dirty();
    if (motion_finished)
        motion_stop(&render->motion);

done:
    core_put_data_pinned(render);
}

void list_render_draw(struct gui_synclist *list)
{
    draw_list(list, false);
}

void gui_synclist_draw_status(struct gui_synclist *list)
{
    if (list->callback_draw_status)
        draw_list(list, true);
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

void list_render_forget(const struct gui_synclist *list)
{
    if (renderer_handle <= 0)
        return;
    struct native_list *render = core_get_data_pinned(renderer_handle);
    if (render->owner == list)
    {
        render->owner = NULL;
        render->has_marquee = false;
        motion_stop(&render->motion);
        memset(&render->overlay, 0, sizeof(render->overlay));
        render->overlay.last_sel = -1;
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

static int motion_deadline(const struct gui_synclist *list, bool poll)
{
    if (renderer_handle <= 0 || ui_scrolling_held())
        return TIMEOUT_BLOCK;

    struct native_list *render = core_get_data_pinned(renderer_handle);
    int delay = TIMEOUT_BLOCK;
    if (motion_owner(render, list))
    {
        const struct list_motion *motion = &render->motion;
        uint32_t now = USEC_TIMER;
        if (motion->visible)
        {
            uint32_t elapsed = now - motion->started;
            if (!motion_enabled() || ui_clock_frame(elapsed) > motion->painted)
                delay = 0;
            else if (poll)
                delay = ui_clock_poll_timeout(elapsed, motion_duration(motion) + 1, HZ);
        }
        if (global_settings.scrolling_enabled && render->has_marquee)
        {
            unsigned frames = marquee_frames();
            unsigned frame = ui_clock_frame(now - render->marquee_started);
            int ticks = frame / frames != render->marquee_frame / frames ?
                        0 : (frames * HZ + UI_ANIM_FPS - 1) / UI_ANIM_FPS;
            if (delay == TIMEOUT_BLOCK || ticks < delay)
                delay = ticks;
        }
        if (render->overlay.visible)
        {
            int32_t remaining = render->overlay.deadline - now;
            int ticks = remaining <= 0 ? 0 :
                        poll ? (int64_t)remaining * HZ / 1000000 : TIMEOUT_BLOCK;
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

int list_render_timeout(const struct gui_synclist *list, int timeout)
{
    int delay = motion_deadline(list, true);
    if (delay != TIMEOUT_BLOCK && (timeout == TIMEOUT_BLOCK || timeout > delay))
        return delay;
    return timeout;
}
