/* Retained local Now Playing. GPL version 2+. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "crc32.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "metadata.h"
#include "rbpaths.h"
#include "draw/screen_access.h"
#include "string-extra.h"
#include "system.h"

#include "audio/play_status.h"
#include "audio/spectrum_meter.h"
#include "metadata/albumart.h"
#include "metadata/tag_trim.h"
#include "playlist/playlist.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "skin/skin_albumart_color.h"
#include "skin/theme_options.h"
#include "system/format_time.h"
#include "screens/playback/cider.h"

#include "native_theme_runtime.h"
#include "native_wps.h"
#include "retained.h"

#if LCD_WIDTH == 320 && LCD_HEIGHT == 240 && LCD_DEPTH == 16 && !defined(HAVE_REMOTE_LCD)

#define SCRIM_WPS WPS_DIR "/scrim.wps"
#define SCRIM_PREFS THEME_DIR "/scrim.preferences"
#define SCRIM_WPS_CRC32 UINT32_C(0x31462e3d)
#define BANDS 8

enum wps_view {
    WPS_ROOT,
    WPS_TRANSPORT,
    WPS_TITLE,
    WPS_ARTIST,
    WPS_PROGRESS,
    WPS_PROGRESS_FILL,
    WPS_INDEX,
    WPS_REMAINING,
    WPS_SPECTRUM,
    WPS_VIEW_COUNT,
};

struct wps_label {
    int font, width;
    const char *text;
    bool right;
};

static struct {
    struct ui_page page;
    struct ui_view view[WPS_VIEW_COUNT];
    struct wps_label title, artist, index, remaining, transport;
    char title_text[MAX_PATH], artist_text[MAX_PATH];
    char index_text[16], remaining_text[24], transport_text[4];
    char path[MAX_PATH];
    unsigned bg, fg, accent, meter_fg, meter_peak;
    int progress_alpha, peak_height;
    int level[BANDS], peak[BANDS];
    long peek_tick, meter_due;
    bool has_art;
    bool active;
    volatile bool repaint;
} wps;

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
        .fg_pattern = wps.fg,
        .bg_pattern = wps.bg,
    };
    return lcd_set_viewport(viewport);
}

static void paint_background(const struct ui_view *view,
                             const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    lcd_set_foreground(wps.bg);
    lcd_fillrect(0, 0, viewport.width, viewport.height);
    lcd_set_viewport(previous);
}

static void paint_label(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct wps_label *label = view->data;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, label->font);
    int x = frame->bounds.x - frame->clip.x;
    if (label->right)
        x += frame->bounds.width - label->width;
    lcd_putsxy(x, frame->bounds.y - frame->clip.y,
               (const unsigned char *)label->text);
    lcd_set_viewport(previous);
}

static void paint_progress(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    int x = frame->bounds.x - frame->clip.x;
    int y = frame->bounds.y - frame->clip.y;
    lcd_blendrect(x, y, frame->bounds.width, frame->bounds.height, wps.progress_alpha);
    lcd_set_viewport(previous);
}

static void paint_progress_fill(const struct ui_view *view,
                                const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    lcd_set_foreground(wps.accent);
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
        lcd_set_foreground(wps.meter_fg);
        lcd_fillrect(x, bottom - wps.level[band], width, wps.level[band]);
        if (wps.peak_height)
        {
            lcd_set_foreground(wps.meter_peak);
            lcd_fillrect(x, bottom - wps.peak[band], width, wps.peak_height);
        }
    }
    lcd_set_viewport(previous);
}

static const struct ui_view_ops background_ops = { .paint = paint_background };
static const struct ui_view_ops label_ops = { .paint = paint_label };
static const struct ui_view_ops progress_ops = { .paint = paint_progress };
static const struct ui_view_ops progress_fill_ops = { .paint = paint_progress_fill };
static const struct ui_view_ops meter_ops = { .paint = paint_meter };

static bool pref_is(const char *key, const char *value, bool allow_unset)
{
    char text[32];
    if (!theme_options_read(SCRIM_PREFS, key, text, sizeof(text)))
        return allow_unset;
    return !strcmp(text, value);
}

/* Preserve custom Scrim files during migration. Missing built-in WPS assets
 * are fine because native rendering does not use the skin. */
static bool built_in_scrim(void)
{
    int fd = open(SCRIM_WPS, O_RDONLY);
    if (fd < 0)
        return !file_exists(SCRIM_WPS);

    char buf[512];
    uint32_t crc = UINT32_MAX;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        crc = crc_32r(buf, n, crc);
    close(fd);
    return n == 0 && (crc ^ UINT32_MAX) == SCRIM_WPS_CRC32;
}

static bool volume_overlay(void)
{
    return global_status.last_volume_change &&
           TIME_BEFORE(current_tick, global_status.last_volume_change + 3 * HZ);
}

static bool can_draw_track(const struct wps_state *state)
{
    const struct mp3entry *track = state->id3;
    if (cider_active() || !track || !track->length || track->length > INT_MAX ||
        track->cuesheet ||
        state->ff_rewind_count || volume_overlay() ||
        (current_playmode() != STATUS_PLAY && current_playmode() != STATUS_PAUSE))
        return false;

    if (strcmp(wps.path, track->path))
    {
        struct albumart_source source;
        strlcpy(wps.path, track->path, sizeof(wps.path));
        wps.has_art = albumart_find_source(track, global_settings.album_art,
                                           false, &source);
    }
    unsigned int dominant, vivid;
    return !wps.has_art && !dynamic_colors_roles(&dominant, &vivid);
}

static struct ui_rect view_bounds(enum wps_view id)
{
    const int *v = wps.view[id].value;
    return (struct ui_rect) { v[UI_X], v[UI_Y], v[UI_WIDTH], v[UI_HEIGHT] };
}

static bool label_update(enum wps_view id, struct wps_label *label,
                         char *buffer, size_t size, const char *text)
{
    if (!text)
        text = "";
    if (!strcmp(buffer, text))
        return true;

    if (strlen(text) >= size)
        return false;
    int width = font_getstringsize((const unsigned char *)text, NULL, NULL, label->font);
    if (width > view_bounds(id).width)
        return false;

    strlcpy(buffer, text, size);
    label->width = width;
    ui_page_invalidate(&wps.page, view_bounds(id));
    return true;
}

static bool refresh_labels(const struct wps_state *state)
{
    char buf[MAX_PATH];
    const struct mp3entry *track = state->id3;
    if (!label_update(WPS_TITLE, &wps.title, wps.title_text, sizeof(wps.title_text),
                      tag_trim(track->title, buf, sizeof(buf))) ||
        !label_update(WPS_ARTIST, &wps.artist, wps.artist_text,
                      sizeof(wps.artist_text), track->artist))
        return false;

    snprintf(buf, sizeof(buf), "%02d", playlist_get_display_index());
    if (!label_update(WPS_INDEX, &wps.index, wps.index_text, sizeof(wps.index_text), buf))
        return false;

    unsigned long length = track->length;
    unsigned long position = MIN(track->elapsed, length);
    format_time(buf, sizeof(buf), (long)(length - position));
    if (!label_update(WPS_REMAINING, &wps.remaining, wps.remaining_text,
                      sizeof(wps.remaining_text), buf))
        return false;

    int i = 0;
    buf[i++] = state->paused ? 'B' : 'A';
    if (global_settings.playlist_shuffle)
        buf[i++] = 'H';
    if (global_settings.repeat_mode >= REPEAT_ALL && global_settings.repeat_mode <= REPEAT_AB)
        buf[i++] = 'J';
    buf[i] = '\0';
    if (!label_update(WPS_TRANSPORT, &wps.transport, wps.transport_text,
                      sizeof(wps.transport_text), buf))
        return false;

    int track_width = view_bounds(WPS_PROGRESS).width;
    /* Use the same overflow-safe ratio as the original progress widget. */
    while (length > (unsigned long)INT_MAX / track_width)
    {
        length >>= 1;
        position >>= 1;
    }
    int width = MAX(1, (int)(position * track_width / length));
    if (wps.view[WPS_PROGRESS_FILL].value[UI_WIDTH] != width)
        ui_page_set(&wps.page, &wps.view[WPS_PROGRESS_FILL], UI_WIDTH, width);
    return true;
}

static void refresh_meter(void)
{
    long now = current_tick;
    if (wps.peek_tick != now)
    {
        wps.peek_tick = now;
        spectrum_meter_peek();
    }
    if (TIME_BEFORE(now, wps.meter_due))
        return;
    wps.meter_due = now + HZ / SPECTRUM_FPS;

    int height = view_bounds(WPS_SPECTRUM).height;
    bool changed = false;
    for (int band = 0; band < BANDS; band++)
    {
        int level = MAX(1, spectrum_meter_get_bar(band, BANDS) * height / 100);
        int peak = MAX(wps.peak_height, spectrum_meter_get_peak(band, BANDS) * height / 100);
        changed |= level != wps.level[band] || peak != wps.peak[band];
        wps.level[band] = level;
        wps.peak[band] = peak;
    }
    if (changed)
        ui_page_invalidate(&wps.page, view_bounds(WPS_SPECTRUM));
}

static unsigned theme_color(const struct ui_theme *theme, enum ui_color_role role)
{
    uint32_t rgb = ui_theme_color(theme, role, NULL);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static void setup_page(const struct ui_theme *theme)
{
    const uint16_t *m = theme->metric;
    wps.bg = theme_color(theme, UI_COLOR_WPS_BG);
    wps.fg = theme_color(theme, UI_COLOR_WPS_FG);
    wps.accent = theme_color(theme, UI_COLOR_WPS_ACCENT);
    wps.meter_fg = theme_color(theme, UI_COLOR_WPS_METER);
    wps.meter_peak = theme_color(theme, UI_COLOR_WPS_MUTED);
    wps.progress_alpha = m[UI_METRIC_WPS_PROGRESS_ALPHA];
    wps.peak_height = m[UI_METRIC_WPS_METER_PEAK_HEIGHT];

    const struct ui_rect bounds[WPS_VIEW_COUNT] = {
        [WPS_ROOT] = { 0, 0, LCD_WIDTH, LCD_HEIGHT },
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
    };

    ui_view_init(&wps.view[WPS_ROOT], bounds[WPS_ROOT], &background_ops, NULL);
    ui_view_init(&wps.view[WPS_TRANSPORT], bounds[WPS_TRANSPORT], &label_ops, &wps.transport);
    ui_view_init(&wps.view[WPS_TITLE], bounds[WPS_TITLE], &label_ops, &wps.title);
    ui_view_init(&wps.view[WPS_ARTIST], bounds[WPS_ARTIST], &label_ops, &wps.artist);
    ui_view_init(&wps.view[WPS_PROGRESS], bounds[WPS_PROGRESS], &progress_ops, NULL);
    ui_view_init(&wps.view[WPS_PROGRESS_FILL], bounds[WPS_PROGRESS_FILL], &progress_fill_ops, NULL);
    ui_view_init(&wps.view[WPS_INDEX], bounds[WPS_INDEX], &label_ops, &wps.index);
    ui_view_init(&wps.view[WPS_REMAINING], bounds[WPS_REMAINING], &label_ops, &wps.remaining);
    ui_view_init(&wps.view[WPS_SPECTRUM], bounds[WPS_SPECTRUM], &meter_ops, NULL);
    for (int i = WPS_ROOT + 1; i < WPS_VIEW_COUNT; i++)
    {
        enum wps_view parent = i == WPS_PROGRESS_FILL ? WPS_PROGRESS : WPS_ROOT;
        ui_view_append(&wps.view[parent], &wps.view[i]);
    }
    ui_page_init(&wps.page, &wps.view[WPS_ROOT]);
}

bool native_wps_open(const struct wps_state *state)
{
    if (!ui_native_enabled() || strcmp(global_settings.wps_file, "scrim") ||
        global_settings.wps_art_source != WPS_ART_ALBUM ||
        global_settings.fg_color != LCD_WHITE ||
        global_settings.bg_color != LCD_BLACK ||
        !built_in_scrim() || !pref_is("sub-line mode", "artist", false) ||
        !pref_is("auto-hide", "off", true) ||
        !pref_is("play-pause icon", "current", true) ||
        !pref_is("spectrum peaks", "on", true))
        return false;

    wps.path[0] = '\0';
    wps.has_art = false;
    if (!can_draw_track(state))
        return false;

    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    /* With no artwork available, all background modes use the base role. */

    /* Save each font reference even if a following load fails. */
    wps.title = (struct wps_label) {
        .font = ui_theme_font_load(&theme, UI_FONT_WPS_TITLE, 48), .text = wps.title_text,
    };
    wps.artist = (struct wps_label) {
        .font = ui_theme_font_load(&theme, UI_FONT_BODY, 48), .text = wps.artist_text,
    };
    wps.index = (struct wps_label) {
        .font = ui_theme_font_load(&theme, UI_FONT_WPS_META, 48), .text = wps.index_text,
    };
    wps.remaining = (struct wps_label) {
        .font = wps.index.font, .text = wps.remaining_text, .right = true,
    };
    wps.transport = (struct wps_label) {
        .font = ui_theme_font_load(&theme, UI_FONT_ICON, 48), .text = wps.transport_text,
    };
    wps.active = true;
    if (wps.title.font < 0 || wps.artist.font < 0 || wps.index.font < 0 || wps.transport.font < 0)
    {
        native_wps_close();
        return false;
    }

    setup_page(&theme);

    wps.title_text[0] = wps.artist_text[0] = '\0';
    wps.index_text[0] = wps.remaining_text[0] = wps.transport_text[0] = '\0';
    wps.peek_tick = current_tick - 1;
    wps.meter_due = current_tick;
    wps.repaint = true;
    if (!refresh_labels(state))
    {
        native_wps_close();
        return false;
    }
    return true;
}

bool native_wps_active(void)
{
    return wps.active;
}

bool native_wps_refresh(const struct wps_state *state)
{
    if (!can_draw_track(state) || !refresh_labels(state))
        return false;
    if (lcd_active())
    {
        if (wps.repaint)
        {
            wps.repaint = false;
            ui_page_invalidate(&wps.page, view_bounds(WPS_ROOT));
        }
        refresh_meter();
        ui_page_draw(&wps.page);
    }
    return true;
}

void native_wps_invalidate(void)
{
    wps.repaint = true;
}

void native_wps_close(void)
{
    if (!wps.active)
        return;
    wps.active = false;
    ui_page_cancel_tracks(&wps.page);
    if (wps.title.font >= 0)
        font_unload(wps.title.font);
    if (wps.artist.font >= 0)
        font_unload(wps.artist.font);
    if (wps.index.font >= 0)
        font_unload(wps.index.font);
    if (wps.transport.font >= 0)
        font_unload(wps.transport.font);
}

#else

bool native_wps_open(const struct wps_state *state)
{
    (void)state;
    return false;
}

bool native_wps_active(void) { return false; }
bool native_wps_refresh(const struct wps_state *state)
{
    (void)state;
    return false;
}
void native_wps_invalidate(void) {}
void native_wps_close(void) {}

#endif
