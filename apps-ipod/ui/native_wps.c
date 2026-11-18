/* Retained Now Playing. GPL version 2+.
 * Scrim geometry and transport glyphs adapted from Anthony Fletcher's
 * PodBox theme (CC BY-SA 3.0).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "core_alloc.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "metadata.h"
#include "sound.h"
#include "system.h"

#include "audio/playback.h"
#include "audio/play_status.h"
#include "audio/spectrum_meter.h"
#include "metadata/album_palette.h"
#include "metadata/tag_trim.h"
#include "playlist/playlist.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "system/format_time.h"

#include "native_theme_runtime.h"
#include "native_wps.h"
#include "perf.h"
#include "retained.h"

#define COVER_EDGE 240
#define BANDS 8
#define SCROLL_PAUSE ANIM_F(60)
#define SCROLL_GAP 24
#define VOLUME_HOLD (ANIM_F(180) * HZ / UI_ANIM_FPS)
#define VALUE_FRAMES ANIM_F(8)
#define VOLUME_OPEN_FRAMES ANIM_F(16)
#define VOLUME_CLOSE_FRAMES ANIM_F(12)

enum wps_view {
    WPS_ROOT, WPS_COVER, WPS_PANEL, WPS_TRANSPORT, WPS_TITLE, WPS_ARTIST,
    WPS_PROGRESS, WPS_PROGRESS_FILL, WPS_INDEX, WPS_REMAINING, WPS_SPECTRUM,
    WPS_VOLUME, WPS_VOLUME_FILL, WPS_VIEW_COUNT,
};

struct wps_label {
    int font, width, offset;
    char *text;
    size_t length, capacity;
    uint32_t scroll_started;
    bool right, scroll;
};

struct wps_progress {
    struct ui_track motion;
    char path[MAX_PATH];
    unsigned long elapsed, length;
    uint32_t clock;
    unsigned seek_serial;
    int target;
    bool remote;
};

struct native_wps {
    struct ui_page page;
    struct ui_view view[WPS_VIEW_COUNT];
    struct ui_theme theme;
    struct wps_label title, artist, index, remaining, transport;
    struct wps_progress progress;
    struct ui_track volume_width, volume_reveal;
    char title_text[MAX_PATH], artist_text[MAX_PATH];
    char index_text[16], remaining_text[24], transport_text[4];
    unsigned bg, fg, accent, panel, meter_fg, meter_peak;
    unsigned palette_generation, art_generation;
    int art_handle, peak_height, volume_target;
    int level[BANDS], peak[BANDS];
    long meter_due;
    unsigned motion_trace;
    bool have_blur, volume_visible, volume_muted, volume_known, remote_meter, motion_tried;
};

static int wps_handle, blur_handle;
static int art_slot = -1;
static struct native_wps *wps;
static volatile bool active, repaint;

void native_wps_alloc(void)
{
    wps_handle = core_alloc_free_space(sizeof(*wps));
    blur_handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    struct dim cover = { .width = COVER_EDGE, .height = COVER_EDGE };
    art_slot = playback_claim_aa_slot(&cover);
    cider_wps_alloc();
}

int native_wps_art_slot(void)
{
    return art_slot;
}

static struct viewport *paint_viewport(struct viewport *viewport,
                                       const struct ui_view_frame *frame,
                                       int font)
{
    *viewport = (struct viewport) {
        .x = frame->clip.x,
        .y = frame->clip.y,
        .width = frame->clip.width,
        .height = frame->clip.height,
        .flags = VP_DEFAULT_FLAGS,
        .font = font,
        .drawmode = DRMODE_FG,
        .buffer = &lcd_framebuffer_default,
        .fg_pattern = wps->fg,
        .bg_pattern = wps->bg,
    };
    return lcd_set_viewport(viewport);
}

static void paint_background(const struct ui_view *view,
                             const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    if (wps->have_blur)
    {
        fb_data *pixels = core_get_data_pinned(blur_handle);
        lcd_bitmap_part(pixels, viewport.x, viewport.y, LCD_WIDTH, 0, 0,
                        viewport.width, viewport.height);
        core_put_data_pinned(pixels);
    }
    else
    {
        lcd_set_foreground(wps->bg);
        lcd_fillrect(0, 0, viewport.width, viewport.height);
    }
    lcd_set_viewport(previous);
}

static void paint_cover(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    if (wps->theme.background[UI_BG_WPS] != UI_BG_COVER_SCRIM ||
        wps->art_handle < 0 || !wps_art_pin(wps->art_handle, true))
        return;

    struct bitmap *art;
    if (wps_art_bitmap(wps->art_handle, &art) && art->data)
    {
        struct viewport viewport;
        struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
        int width = MIN(frame->bounds.width, art->width);
        int height = MIN(frame->bounds.height, art->height);
        int x = frame->bounds.x + (frame->bounds.width - width) / 2 - frame->clip.x;
        int y = frame->bounds.y + (frame->bounds.height - height) / 2 - frame->clip.y;
        /* Audio buffering returns RGB565 data, but its bitmap format field
         * describes the decoder, not the display primitive. */
        lcd_bitmap_part((const fb_data *)art->data, (art->width - width) / 2,
                        (art->height - height) / 2, art->width,
                        x, y, width, height);
        lcd_set_viewport(previous);
    }
    wps_art_pin(wps->art_handle, false);
}

static void paint_panel(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    if (wps->art_handle < 0 || wps->theme.background[UI_BG_WPS] == UI_BG_SOLID)
        return;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    lcd_set_foreground(wps->panel);
    lcd_blendrect(0, 0, viewport.width, viewport.height,
                  wps->theme.metric[UI_METRIC_WPS_PANEL_ALPHA]);
    lcd_set_viewport(previous);
}

static void paint_label(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct wps_label *label = view->data;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, label->font);
    int x = frame->bounds.x - frame->clip.x - label->offset;
    int y = frame->bounds.y - frame->clip.y;
    if (label->right)
        x += frame->bounds.width - label->width;
    lcd_putsxy(x, y, (const unsigned char *)label->text);
    if (label->scroll && label->width > frame->bounds.width)
        lcd_putsxy(x + label->width + SCROLL_GAP, y,
                   (const unsigned char *)label->text);
    lcd_set_viewport(previous);
}

static void paint_progress(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    lcd_blendrect(0, 0, viewport.width, viewport.height,
                  wps->theme.metric[UI_METRIC_WPS_PROGRESS_ALPHA]);
    lcd_set_viewport(previous);
}

static void paint_progress_fill(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    lcd_set_foreground(wps->accent);
    lcd_fillrect(0, 0, viewport.width, viewport.height);
    lcd_set_viewport(previous);
}

static void paint_meter(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    int x = frame->bounds.x - frame->clip.x;
    int bottom = frame->bounds.y - frame->clip.y + frame->bounds.height;
    int width = frame->bounds.width / BANDS;
    for (int band = 0; band < BANDS; band++, x += width)
    {
        lcd_set_foreground(wps->meter_fg);
        lcd_fillrect(x, bottom - wps->level[band], width, wps->level[band]);
        if (wps->peak_height)
        {
            lcd_set_foreground(wps->meter_peak);
            lcd_fillrect(x, bottom - wps->peak[band], width, wps->peak_height);
        }
    }
    lcd_set_viewport(previous);
}

static void paint_volume(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    const uint16_t *m = wps->theme.metric;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, wps->transport.font);
    lcd_set_foreground(wps->bg);
    lcd_fillrect(0, 0, viewport.width, viewport.height);
    int x = m[UI_METRIC_WPS_VOLUME_X] - frame->clip.x;
    int layer_y = frame->bounds.y - frame->clip.y;
    int y = m[UI_METRIC_WPS_VOLUME_Y] + layer_y;
    int width = m[UI_METRIC_WPS_VOLUME_WIDTH];
    int height = m[UI_METRIC_WPS_VOLUME_HEIGHT];
    lcd_set_foreground(wps->meter_peak);
    lcd_fillrect(x, y, width, height);
    lcd_set_foreground(wps->fg);
    y = m[UI_METRIC_WPS_TRANSPORT_Y] + layer_y + 4;
    lcd_putsxy(m[UI_METRIC_STATUS_PAD_LEFT] - frame->clip.x, y,
               (const unsigned char *)(wps->volume_muted ? "M" : "N"));
    lcd_putsxy(LCD_WIDTH - frame->clip.x - m[UI_METRIC_STATUS_PAD_RIGHT] -
               m[UI_METRIC_STATUS_ICON_SIZE], y, (const unsigned char *)"O");
    lcd_set_viewport(previous);
}

static const struct ui_view_ops background_ops = { .paint = paint_background };
static const struct ui_view_ops cover_ops = { .paint = paint_cover };
static const struct ui_view_ops panel_ops = { .paint = paint_panel };
static const struct ui_view_ops label_ops = { .paint = paint_label };
static const struct ui_view_ops progress_ops = { .paint = paint_progress };
static const struct ui_view_ops progress_fill_ops = { .paint = paint_progress_fill };
static const struct ui_view_ops meter_ops = { .paint = paint_meter };
static const struct ui_view_ops volume_ops = { .paint = paint_volume };

static struct ui_rect view_bounds(enum wps_view id)
{
    const int *v = wps->view[id].value;
    return (struct ui_rect) { v[UI_X], v[UI_Y], v[UI_WIDTH], v[UI_HEIGHT] };
}

static void label_set(struct wps_label *label, int font, char *text, size_t capacity)
{
    *label = (struct wps_label) { .font = font, .text = text, .capacity = capacity };
}

static void label_update(enum wps_view id, struct wps_label *label, const char *text,
                         uint32_t now)
{
    if (!text)
        text = "";
    size_t len = 0;
    while (len < label->capacity && text[len])
        ++len;
    if (len == label->capacity)
    {
        /* Preserve the UTF-8 boundary so a long tag can keep scrolling. */
        --len;
        while (len && ((unsigned char)text[len] & 0xc0) == 0x80)
            --len;
    }
    if (label->length != len || memcmp(label->text, text, len))
    {
        memcpy(label->text, text, len);
        label->text[len] = '\0';
        label->length = len;
        label->width = font_getstringsize((const unsigned char *)label->text, NULL,
                                         NULL, label->font);
        label->scroll_started = now;
        label->offset = 0;
        ui_page_invalidate(&wps->page, view_bounds(id));
    }
    struct ui_rect bounds = view_bounds(id);
    if (label->scroll && label->width > bounds.width)
    {
        unsigned frame = ui_clock_frame(now - label->scroll_started);
        unsigned distance = (unsigned)label->width + SCROLL_GAP;
        unsigned loop = frame % (SCROLL_PAUSE + distance);
        int offset = loop < SCROLL_PAUSE ? 0 : (int)(loop - SCROLL_PAUSE);
        if (label->offset != offset)
        {
            label->offset = offset;
            ui_page_invalidate(&wps->page, bounds);
        }
    }
}

static void refresh_progress(const struct wps_state *state, bool remote, uint32_t now,
                             unsigned long position, int play_mode)
{
    struct wps_progress *progress = &wps->progress;
    const struct mp3entry *track = state->id3;
    unsigned seek_serial = remote ? 0 : audio_seek_serial();
    unsigned long elapsed = track ? track->elapsed : 0;
    unsigned long length = track ? track->length : 0;
    bool same_track = length && progress->length == length && remote == progress->remote &&
                      !strcmp(progress->path, track->path);
    if (!same_track)
        snprintf(progress->path, sizeof(progress->path), "%s", track ? track->path : "");

    bool preview = state->ff_rewind_count || play_mode == STATUS_FASTFORWARD ||
                   play_mode == STATUS_FASTBACKWARD;
    int64_t advance = (int64_t)elapsed - (int64_t)progress->elapsed;
    /* Remote position samples are extrapolated on the iPod. Allow twice the
     * wall advance and 12 f of clock correction before classifying a seek. */
    uint32_t allowance = (now - progress->clock) / 500 + ui_frame_clock(ANIM_F(12)) / 1000;
    bool landed = seek_serial != progress->seek_serial ||
                  (remote && (advance > allowance || advance < -(int64_t)allowance));
    bool motion = same_track && (preview || landed);
    progress->seek_serial = seek_serial;
    progress->remote = remote;
    progress->elapsed = elapsed;
    progress->length = length;
    progress->clock = now;

    struct ui_view *fill = &wps->view[WPS_PROGRESS_FILL];
    int track_width = view_bounds(WPS_PROGRESS).width;
    /* Scale before multiplication so this also works on 32-bit targets. */
    while (length > (unsigned long)INT_MAX / track_width)
    {
        length >>= 1;
        position >>= 1;
    }
    int width = length ? MAX(1, (int)(position * track_width / length)) : 0;
    if (!same_track)
    {
        ui_page_set(&wps->page, fill, UI_WIDTH, width);
        progress->motion.tween = (struct ui_tween){0};
    }
    else if (width == progress->target)
        return;
    else if (motion)
        ui_page_tween(&wps->page, &progress->motion, fill, UI_WIDTH, width,
                      VALUE_FRAMES, false, now);
    else if (ui_clock_frame(now - progress->motion.tween.start_clock) <
             progress->motion.tween.frames)
        return;
    else
        ui_page_set(&wps->page, fill, UI_WIDTH, width);
    progress->target = width;
}

static void refresh_labels(const struct wps_state *state, const struct cider_status *source,
                           bool remote, uint32_t now)
{
    char buf[MAX_PATH];
    const struct mp3entry *track = state->id3;
    const char *title = "";
    if (track && track->title && track->title[0])
        title = tag_trim(track->title, buf, sizeof(buf));
    else if (track && !remote)
    {
        const char *name = strrchr(track->path, '/');
        title = name ? name + 1 : track->path;
    }
    label_update(WPS_TITLE, &wps->title, title, now);
    label_update(WPS_ARTIST, &wps->artist, track ? track->artist : NULL, now);

    snprintf(buf, sizeof(buf), "%02d", remote ? 1 : MAX(0, playlist_get_display_index()));
    label_update(WPS_INDEX, &wps->index, buf, now);

    unsigned long length = track ? track->length : 0;
    /* Preview offsets are signed. Clamp both ends before deriving labels. */
    int64_t preview = track ? (int64_t)track->elapsed + state->ff_rewind_count : 0;
    unsigned long position = preview < 0 ? 0 : MIN((uint64_t)preview, length);
    format_time(buf, sizeof(buf), (long)MIN(length - position, LONG_MAX));
    label_update(WPS_REMAINING, &wps->remaining, buf, now);

    int playmode = current_playmode();
    int i = 0;
    if (playmode == STATUS_FASTFORWARD)
        buf[i++] = 'G';
    else if (playmode == STATUS_FASTBACKWARD)
        buf[i++] = 'F';
    else if (source->loading)
        buf[i++] = 'V';
    else if (!track || (remote && !source->available))
        buf[i++] = 'C';
    else
        buf[i++] = state->paused ? 'B' : 'A';
    if (remote ? cider_mode_get(CIDER_MODE_SHUFFLE) : global_settings.playlist_shuffle)
        buf[i++] = 'H';
    if (remote ? cider_mode_get(CIDER_MODE_REPEAT) : global_settings.repeat_mode)
        buf[i++] = 'J';
    buf[i] = '\0';
    label_update(WPS_TRANSPORT, &wps->transport, buf, now);

    refresh_progress(state, remote, now, position, playmode);
}

static void refresh_meter(bool remote)
{
    long now = current_tick;
    if (wps->remote_meter != remote)
    {
        spectrum_meter_reset();
        wps->remote_meter = remote;
    }
    if (TIME_BEFORE(now, wps->meter_due))
        return;
    wps->meter_due = now + HZ / SPECTRUM_FPS;
    if (remote)
    {
        unsigned char levels[SPECTRUM_MAX_BANDS * 2];
        spectrum_meter_remote(cider_spectrum_levels(levels) ? levels : NULL);
    }
    else
        spectrum_meter_peek();

    int height = view_bounds(WPS_SPECTRUM).height;
    bool changed = false;
    for (int band = 0; band < BANDS; band++)
    {
        int level = MAX(1, spectrum_meter_get_bar(band, BANDS) * height / 100);
        int peak = MAX(wps->peak_height, spectrum_meter_get_peak(band, BANDS) * height / 100);
        changed |= level != wps->level[band] || peak != wps->peak[band];
        wps->level[band] = level;
        wps->peak[band] = peak;
    }
    if (changed)
        ui_page_invalidate(&wps->page, view_bounds(WPS_SPECTRUM));
}

static unsigned theme_color(enum ui_color_role role, const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(&wps->theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static void refresh_art(void)
{
    album_palette_refresh();
    unsigned palette_generation = album_palette_generation();
    unsigned art_generation = album_art_generation();
    int handle = album_art_current();
    bool colors_changed = palette_generation != wps->palette_generation;
    if (colors_changed)
    {
        struct ui_album_palette palette;
        album_palette_snapshot(&palette);
        wps->palette_generation = palette_generation;
        wps->bg = theme_color(UI_COLOR_WPS_BG, &palette);
        wps->fg = theme_color(UI_COLOR_WPS_FG, &palette);
        wps->accent = theme_color(UI_COLOR_WPS_ACCENT, &palette);
        wps->panel = theme_color(UI_COLOR_WPS_PANEL, &palette);
        wps->meter_fg = theme_color(UI_COLOR_WPS_METER, &palette);
        wps->meter_peak = theme_color(UI_COLOR_WPS_MUTED, &palette);
    }
    if (wps->art_generation != art_generation || wps->art_handle != handle || colors_changed)
    {
        wps->art_generation = art_generation;
        wps->art_handle = handle;
        wps->have_blur = false;
        if (wps->theme.background[UI_BG_WPS] == UI_BG_BLURRED_COVER &&
            handle >= 0 && blur_handle > 0)
        {
            fb_data *pixels = core_get_data_pinned(blur_handle);
            wps->have_blur = album_art_render_background(pixels, LCD_WIDTH, LCD_HEIGHT,
                                                         handle, true, wps->fg);
            core_put_data_pinned(pixels);
        }
        ui_page_invalidate(&wps->page, view_bounds(WPS_ROOT));
    }
}

static void refresh_volume(const struct cider_status *source, bool remote, uint32_t now)
{
    bool visible = global_status.last_volume_change &&
        TIME_BEFORE(current_tick, global_status.last_volume_change + VOLUME_HOLD);
    int min = sound_min(SOUND_VOLUME);
    int max = sound_max(SOUND_VOLUME);
    int percent = remote ? source->volume :
        (global_status.volume - min) * 100 / MAX(1, max - min);
    percent = MIN(100, MAX(0, percent));
    bool muted = remote ? source->muted : global_status.volume <= min;
    int target = wps->theme.metric[UI_METRIC_WPS_VOLUME_WIDTH] * percent / 100;
    struct ui_view *overlay = &wps->view[WPS_VOLUME];
    struct ui_view *fill = &wps->view[WPS_VOLUME_FILL];
    if (!wps->volume_known || target != wps->volume_target)
    {
        if (visible && wps->volume_known)
            ui_page_tween(&wps->page, &wps->volume_width, fill, UI_WIDTH, target,
                          VALUE_FRAMES, false, now);
        else
            ui_page_set(&wps->page, fill, UI_WIDTH, target);
        wps->volume_target = target;
        wps->volume_known = true;
    }
    if (wps->volume_visible != visible)
    {
        int offset = visible ? 0 : -overlay->value[UI_HEIGHT];
        ui_page_tween(&wps->page, &wps->volume_reveal, overlay, UI_OFFSET_Y, offset,
                      visible ? VOLUME_OPEN_FRAMES : VOLUME_CLOSE_FRAMES, !visible, now);
        wps->volume_visible = visible;
    }
    if (wps->volume_muted != muted)
    {
        wps->volume_muted = muted;
        if (visible)
            ui_page_invalidate(&wps->page, view_bounds(WPS_VOLUME));
    }
}

static void setup_page(void)
{
    const uint16_t *m = wps->theme.metric;
    int cover = MIN(m[UI_METRIC_COVER_SIZE], MIN(LCD_WIDTH, LCD_HEIGHT));
    wps->peak_height = m[UI_METRIC_WPS_METER_PEAK_HEIGHT];
    const struct ui_rect bounds[WPS_VIEW_COUNT] = {
        [WPS_ROOT] = { 0, 0, LCD_WIDTH, LCD_HEIGHT },
        [WPS_COVER] = { LCD_WIDTH - cover, (LCD_HEIGHT - cover) / 2, cover, cover },
        [WPS_PANEL] = { 0, m[UI_METRIC_WPS_PANEL_TOP], m[UI_METRIC_WPS_PROGRESS_WIDTH],
                        MAX(0, m[UI_METRIC_WPS_PROGRESS_Y] - m[UI_METRIC_WPS_PANEL_TOP]) },
        [WPS_TRANSPORT] = { m[UI_METRIC_WPS_TRANSPORT_X], m[UI_METRIC_WPS_TRANSPORT_Y],
                            m[UI_METRIC_WPS_TRANSPORT_WIDTH], m[UI_METRIC_WPS_TRANSPORT_HEIGHT] },
        [WPS_TITLE] = { m[UI_METRIC_WPS_TITLE_X], m[UI_METRIC_WPS_TITLE_Y],
                        m[UI_METRIC_WPS_TITLE_WIDTH], m[UI_METRIC_WPS_TITLE_HEIGHT] },
        [WPS_ARTIST] = { m[UI_METRIC_WPS_TITLE_X], m[UI_METRIC_WPS_SUBTITLE_Y],
                         m[UI_METRIC_WPS_TITLE_WIDTH], m[UI_METRIC_WPS_SUBTITLE_HEIGHT] },
        [WPS_PROGRESS] = { 0, m[UI_METRIC_WPS_PROGRESS_Y],
                            m[UI_METRIC_WPS_PROGRESS_WIDTH], m[UI_METRIC_WPS_PROGRESS_HEIGHT] },
        [WPS_PROGRESS_FILL] = { 0, 0, 1, m[UI_METRIC_WPS_PROGRESS_HEIGHT] },
        [WPS_INDEX] = { m[UI_METRIC_WPS_DECK_INDEX_X], m[UI_METRIC_WPS_DECK_Y],
                        m[UI_METRIC_WPS_DECK_WIDTH], m[UI_METRIC_WPS_DECK_HEIGHT] },
        [WPS_REMAINING] = { m[UI_METRIC_WPS_DECK_TIME_X], m[UI_METRIC_WPS_DECK_Y],
                            m[UI_METRIC_WPS_DECK_WIDTH], m[UI_METRIC_WPS_DECK_HEIGHT] },
        [WPS_SPECTRUM] = { m[UI_METRIC_WPS_METER_X], m[UI_METRIC_WPS_METER_Y],
                            m[UI_METRIC_WPS_METER_WIDTH], m[UI_METRIC_WPS_METER_HEIGHT] },
        [WPS_VOLUME] = { 0, 0, LCD_WIDTH, MAX(m[UI_METRIC_STATUS_HEIGHT],
            m[UI_METRIC_WPS_VOLUME_Y] + m[UI_METRIC_WPS_VOLUME_HEIGHT]) },
        [WPS_VOLUME_FILL] = { m[UI_METRIC_WPS_VOLUME_X], m[UI_METRIC_WPS_VOLUME_Y],
                              0, m[UI_METRIC_WPS_VOLUME_HEIGHT] },
    };

    ui_view_init(&wps->view[WPS_ROOT], bounds[WPS_ROOT], &background_ops, NULL);
    ui_view_init(&wps->view[WPS_COVER], bounds[WPS_COVER], &cover_ops, NULL);
    ui_view_init(&wps->view[WPS_PANEL], bounds[WPS_PANEL], &panel_ops, NULL);
    ui_view_init(&wps->view[WPS_TRANSPORT], bounds[WPS_TRANSPORT], &label_ops, &wps->transport);
    ui_view_init(&wps->view[WPS_TITLE], bounds[WPS_TITLE], &label_ops, &wps->title);
    ui_view_init(&wps->view[WPS_ARTIST], bounds[WPS_ARTIST], &label_ops, &wps->artist);
    ui_view_init(&wps->view[WPS_PROGRESS], bounds[WPS_PROGRESS], &progress_ops, NULL);
    ui_view_init(&wps->view[WPS_PROGRESS_FILL], bounds[WPS_PROGRESS_FILL], &progress_fill_ops, NULL);
    ui_view_init(&wps->view[WPS_INDEX], bounds[WPS_INDEX], &label_ops, &wps->index);
    ui_view_init(&wps->view[WPS_REMAINING], bounds[WPS_REMAINING], &label_ops, &wps->remaining);
    ui_view_init(&wps->view[WPS_SPECTRUM], bounds[WPS_SPECTRUM], &meter_ops, NULL);
    ui_view_init(&wps->view[WPS_VOLUME], bounds[WPS_VOLUME], &volume_ops, NULL);
    ui_view_init(&wps->view[WPS_VOLUME_FILL], bounds[WPS_VOLUME_FILL], &progress_fill_ops, NULL);
    wps->view[WPS_VOLUME].value[UI_OFFSET_Y] = -bounds[WPS_VOLUME].height;
    for (int i = WPS_ROOT + 1; i < WPS_VIEW_COUNT; i++)
    {
        enum wps_view parent = i == WPS_PROGRESS_FILL ? WPS_PROGRESS :
                               i == WPS_VOLUME_FILL ? WPS_VOLUME : WPS_ROOT;
        ui_view_append(&wps->view[parent], &wps->view[i]);
    }
    ui_page_init(&wps->page, &wps->view[WPS_ROOT]);
}

void native_wps_open(const struct wps_state *state)
{
    (void)state;
    if (wps_handle <= 0 || active)
        return;
    wps = core_get_data_pinned(wps_handle);
    memset(wps, 0, sizeof(*wps));
    ui_theme_snapshot(&wps->theme);
    wps->art_handle = -1;
    wps->art_generation = wps->palette_generation = (unsigned)-1;
    wps->meter_due = current_tick;

    label_set(&wps->title, ui_theme_font_load(&wps->theme, UI_FONT_WPS_TITLE, 48),
              wps->title_text, sizeof(wps->title_text));
    label_set(&wps->artist, ui_theme_font_load(&wps->theme, UI_FONT_BODY, 48),
              wps->artist_text, sizeof(wps->artist_text));
    label_set(&wps->index, ui_theme_font_load(&wps->theme, UI_FONT_WPS_META, 48),
              wps->index_text, sizeof(wps->index_text));
    label_set(&wps->remaining, wps->index.font, wps->remaining_text, sizeof(wps->remaining_text));
    label_set(&wps->transport, ui_theme_font_load(&wps->theme, UI_FONT_ICON, 48),
              wps->transport_text, sizeof(wps->transport_text));

    wps->remaining.right = true;
    wps->title.scroll = wps->artist.scroll = true;
    setup_page();
    cider_wps_enter();
    repaint = true;
    active = true;
}

bool native_wps_active(void)
{
    return active;
}

bool native_wps_animating(void)
{
    return active && wps->page.tracks;
}

void native_wps_render(const struct wps_state *state, bool submit)
{
    if (!active)
        return;

    bool remote = cider_active();
    bool visible = lcd_active();
    cider_spectrum_wps(visible && remote);
    if (!visible)
    {
        ui_perf_end(wps->motion_trace);
        wps->motion_trace = 0;
        wps->motion_tried = false;
        return;
    }

    struct cider_status source = {0};
    if (remote)
        cider_status(&source);
    refresh_art();
    uint32_t now = USEC_TIMER;
    refresh_labels(state, &source, remote, now);
    refresh_volume(&source, remote, now);
    if (wps->page.tracks && !wps->motion_tried)
    {
        const char *route = wps->page.tracks == &wps->progress.motion
                            ? "wps-seek-progress" : "wps-volume";
        wps->motion_trace = ui_perf_begin(route);
        wps->motion_tried = true;
    }
    ui_page_step(&wps->page, now);
    refresh_meter(remote);
    if (repaint || !submit)
    {
        repaint = false;
        ui_page_invalidate(&wps->page, view_bounds(WPS_ROOT));
    }
    if (submit)
        ui_page_draw(&wps->page);
    else
        ui_page_paint(&wps->page);
    if (!wps->page.tracks && wps->motion_tried)
    {
        ui_perf_end(wps->motion_trace);
        wps->motion_trace = 0;
        wps->motion_tried = false;
    }
    if (remote)
        cider_loading_drawn(source.loading);
}

void native_wps_invalidate(void)
{
    repaint = true;
}

void native_wps_close(void)
{
    if (!active)
        return;
    active = false;
    ui_perf_end(wps->motion_trace);
    ui_page_cancel_tracks(&wps->page);
    if (wps->title.font >= 0)
        font_unload(wps->title.font);
    if (wps->artist.font >= 0)
        font_unload(wps->artist.font);
    if (wps->index.font >= 0)
        font_unload(wps->index.font);
    if (wps->transport.font >= 0)
        font_unload(wps->transport.font);
    core_put_data_pinned(wps);
    wps = NULL;
}
