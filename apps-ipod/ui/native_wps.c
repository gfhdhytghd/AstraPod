/* Retained Now Playing. GPL version 2+.
 * Scrim geometry and transport glyphs adapted from Anthony Fletcher's
 * PodBox theme (CC BY-SA 3.0).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "plugins/rock_compat.h"
#include "core_alloc.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "metadata.h"
#include "sound.h"
#include "string-extra.h"
#include "system.h"

#include "audio/playback.h"
#include "audio/buffering.h"
#include "audio/play_status.h"
#include "audio/spectrum_meter.h"
#include "metadata/album_palette.h"
#include "metadata/album_blur.h"
#include "metadata/tag_trim.h"
#include "playlist/playlist.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "system/format_time.h"
#include "system/volume.h"

#include "native_chrome.h"
#include "native_theme_runtime.h"
#include "native_wps.h"
#include "perf.h"
#include "retained.h"
#include "theme_art.h"

#define COVER_EDGE 240
#define BANDS 8
#define SCROLL_PAUSE ANIM_F(60)
#define SCROLL_GAP 24
#define VOLUME_HOLD_FRAMES ANIM_F(80)
#define VALUE_FRAMES ANIM_F(8)
#define VOLUME_OPEN_FRAMES ANIM_F(16)
#define VOLUME_CLOSE_FRAMES ANIM_F(12)

/* Classic geometry, from the stock firmware's Now Playing at 320x240. The
 * cover turns away on its vertical axis: its near (left) edge is
 * CLASSIC_ART_NEAR rows tall, its far edge CLASSIC_ART_FAR rows, starting
 * CLASSIC_ART_DROP rows lower. A blank row separates it from a reflection
 * that fades out over CLASSIC_REFLECTION rows. */
#define CLASSIC_ART_X 9
#define CLASSIC_ART_Y 33
#define CLASSIC_ART_WIDTH 136
#define CLASSIC_ART_NEAR 135
#define CLASSIC_ART_FAR 119
#define CLASSIC_ART_DROP 10
#define CLASSIC_REFLECTION 68
#define CLASSIC_REFLECTION_ALPHA 15 /* of 32 */
#define CLASSIC_TEXT_X 158
#define CLASSIC_TEXT_WIDTH 156
#define CLASSIC_TITLE_Y 68
#define CLASSIC_TITLE_HEIGHT 24
#define CLASSIC_ARTIST_Y 90
#define CLASSIC_ALBUM_Y 108
#define CLASSIC_INDEX_Y 145
#define CLASSIC_LINE 17
/* The progress and volume rows share one strip and slide through it. */
#define CLASSIC_ROW_Y 203
#define CLASSIC_ROW_HEIGHT 30
#define CLASSIC_BAR_Y 4
#define CLASSIC_BAR_HEIGHT 14
#define CLASSIC_BAR_REFLECTION 11
#define CLASSIC_BAR_VIEW (CLASSIC_BAR_HEIGHT + 1 + CLASSIC_BAR_REFLECTION)
#define CLASSIC_TIME_Y 6
#define CLASSIC_PROGRESS_X 54
#define CLASSIC_PROGRESS_WIDTH 212
#define CLASSIC_ELAPSED_X 14
#define CLASSIC_REMAINING_RIGHT 310
#define CLASSIC_VOLUME_X 56
#define CLASSIC_VOLUME_WIDTH 208
#define CLASSIC_QUIET_X 37
#define CLASSIC_LOUD_X 274
#define CLASSIC_SPEAKER_Y 12
#define CLASSIC_SHUFFLE_X 292
#define CLASSIC_SHUFFLE_Y 25
/* 4.0 s and 270 ms at the panel's 66.96 Hz. */
#define CLASSIC_VOLUME_HOLD ANIM_F(268)
#define CLASSIC_SLIDE_FRAMES ANIM_F(18)

enum wps_view {
    WPS_ROOT, WPS_COVER, WPS_PANEL, WPS_TITLE, WPS_ARTIST,
    WPS_PROGRESS, WPS_PROGRESS_FILL, WPS_INDEX, WPS_REMAINING, WPS_SPECTRUM,
    WPS_VOLUME, WPS_VOLUME_FILL, WPS_HEADER, WPS_ALBUM, WPS_ELAPSED,
    WPS_ROW, WPS_TIMELINE, WPS_SHUFFLE, WPS_VIEW_COUNT,
};

struct wps_label {
    int font, width, offset;
    char *text;
    size_t length, capacity;
    uint32_t scroll_started;
    bool right, scroll, muted;
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
    struct wps_label title, artist, album, index, elapsed, remaining;
    struct wps_progress progress;
    struct ui_track volume_width, volume_reveal, blur_reveal;
    struct native_chrome chrome;
    char title_text[MAX_PATH], artist_text[MAX_PATH], album_text[MAX_PATH];
    char index_text[32], elapsed_text[24], remaining_text[24];
    unsigned bg, fg, accent, panel, meter_fg, meter_peak;
    uint32_t accent_rgb, muted_rgb;
    unsigned palette_generation, art_generation, blur_generation;
    int art_handle, background, peak_height, volume_target, volume_font;
    int progress_span, volume_span;
    int level[BANDS], peak[BANDS];
    long meter_due;
    unsigned motion_trace;
    bool volume_visible, volume_muted, volume_known, remote_meter, motion_tried;
    bool current_art_only, cover_backdrop, wait_for_blur, labels_frozen;
    bool classic, chrome_ready;
    void (*landing)(const struct ui_rect *clip);
    unsigned reveal_base;
    char cover_remote_key[65];
};

static int wps_handle, blur_handle;
static struct {
    int art;
    unsigned generation, blur_generation, foreground;
    bool ready;
} backdrop = { .art = -1 };
static int art_slot = -1;
static struct native_wps *wps;
static volatile bool active, repaint;
static bool painted_art;

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

int native_wps_background(int art, unsigned generation, enum ui_bg_kind mode, unsigned fg)
{
    if (mode == UI_BG_SOLID || art < 0 || blur_handle <= 0)
        return -1;

    unsigned blur_generation = album_blur_generation();
    if (backdrop.art != art || backdrop.generation != generation ||
        backdrop.foreground != fg || backdrop.blur_generation != blur_generation)
    {
        backdrop.art = art;
        backdrop.generation = generation;
        backdrop.foreground = fg;
        backdrop.blur_generation = blur_generation;
        fb_data *pixels = core_get_data_pinned(blur_handle);
        backdrop.ready = album_blur_render_wps(pixels, art, generation, fg);
        core_put_data_pinned(pixels);
        if (!backdrop.ready)
            album_blur_request_wps();
    }
    return backdrop.ready ? blur_handle : -1;
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

void native_wps_paint_background(const struct viewport *vp, int background, unsigned bg)
{
    if (background > 0)
    {
        fb_data *pixels = core_get_data_pinned(background);
        lcd_bitmap_part(pixels, vp->x, vp->y, LCD_WIDTH, 0, 0, vp->width, vp->height);
        core_put_data_pinned(pixels);
    }
    else
    {
        lcd_set_foreground(bg);
        lcd_fillrect(0, 0, vp->width, vp->height);
    }
}

/* The first playback blur can arrive after the slide. Reveal it beneath
 * the live controls, starting at the exact solid background on the LCD. */
static void reveal_background(int width, int height)
{
    int brightness = wps->view[WPS_ROOT].value[UI_BRIGHTNESS];
    if (brightness >= UI_FULL_OPACITY)
        return;
    unsigned alpha = 15 - (brightness * 15 + UI_FULL_OPACITY / 2) /
                          UI_FULL_OPACITY;
    lcd_set_foreground(wps->reveal_base);
    if (alpha == 15)
        lcd_fillrect(0, 0, width, height);
    else
        lcd_blendrect(0, 0, width, height, alpha);
}

static void paint_background(const struct ui_view *view,
                             const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
    native_wps_paint_background(&viewport, wps->background, wps->bg);
    reveal_background(viewport.width, viewport.height);
    lcd_set_viewport(previous);
}

void native_wps_set_landing(void (*paint)(const struct ui_rect *clip))
{
    wps->landing = paint;
}

static void paint_landing(const struct ui_view_frame *frame)
{
    painted_art = wps->landing != NULL;
    if (painted_art)
        wps->landing(&frame->clip);
}

static void paint_cover(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    painted_art = false;
    if (wps->theme.background[UI_BG_WPS] != UI_BG_COVER_SCRIM)
        return;
    if (wps->art_handle < 0 || !wps_art_pin(wps->art_handle, true))
    {
        paint_landing(frame);
        return;
    }

    struct bitmap *art;
    if (wps_art_bitmap(wps->art_handle, &art) && art->data)
    {
        struct viewport viewport;
        struct viewport *previous = paint_viewport(&viewport, frame, FONT_SYSFIXED);
        int width = MIN(frame->bounds.width, art->width);
        int height = MIN(frame->bounds.height, art->height);
        int x = frame->bounds.x + (frame->bounds.width - width) / 2 - frame->clip.x;
        int y = frame->bounds.y + (frame->bounds.height - height) / 2 - frame->clip.y;
        painted_art = width > 0 && height > 0;
        /* Audio buffering returns RGB565 data, but its bitmap format field
         * describes the decoder, not the display primitive. */
        lcd_bitmap_part((const fb_data *)art->data, (art->width - width) / 2,
                        (art->height - height) / 2, art->width,
                        x, y, width, height);
        lcd_set_viewport(previous);
    }
    wps_art_pin(wps->art_handle, false);
    if (!painted_art)
        paint_landing(frame);
    else
        wps->landing = NULL;
}

bool native_wps_has_art(void)
{
    return active && painted_art;
}

static void paint_panel(const struct ui_view *view, const struct ui_view_frame *frame)
{
    (void)view;
    int mode = wps->theme.background[UI_BG_WPS];
    if (mode == UI_BG_SOLID ||
        (mode != UI_BG_COVER_SCRIM && wps->art_handle < 0))
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
    if (label->muted)
        viewport.fg_pattern = wps->meter_peak;
    int x = frame->bounds.x - frame->clip.x - label->offset;
    int y = frame->bounds.y - frame->clip.y;
    if (label->right)
        x += frame->bounds.width - label->width;
    const unsigned char *text = (const unsigned char *)label->text;
    bool overflow = label->scroll && label->width > frame->bounds.width;
    if (overflow && !global_settings.scrolling_enabled)
    {
        static const unsigned char dots[] = "...";
        int available = frame->bounds.width -
                        font_getstringsize(dots, NULL, NULL, label->font);
        if (available > 0)
        {
            int prefix_width;
            int bytes = font_measurestring(text, label->length, available,
                                           &prefix_width, NULL, label->font);
            char prefix[MAX_PATH + 1];
            size_t end = MIN((size_t)bytes, sizeof(prefix) - 1);
            while (end && (text[end] & 0xc0) == 0x80)
                --end;
            memcpy(prefix, text, end);
            prefix[end] = '\0';
            lcd_putsxy(x, y, (const unsigned char *)prefix);
            if (end != (size_t)bytes)
                prefix_width = font_getstringsize((const unsigned char *)prefix,
                                                  NULL, NULL, label->font);
            x += prefix_width;
        }
        lcd_putsxy(x, y, dots);
    }
    else
    {
        lcd_putsxy(x, y, text);
        if (overflow)
            lcd_putsxy(x + label->width + SCROLL_GAP, y, text);
    }
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
    struct viewport *previous = paint_viewport(&viewport, frame, wps->volume_font);
    lcd_set_foreground(wps->bg);
    lcd_fillrect(0, 0, viewport.width, viewport.height);
    int left = frame->bounds.x - frame->clip.x;
    int x = left + m[UI_METRIC_WPS_VOLUME_X];
    int layer_y = frame->bounds.y - frame->clip.y;
    int y = m[UI_METRIC_WPS_VOLUME_Y] + layer_y;
    int width = m[UI_METRIC_WPS_VOLUME_WIDTH];
    int height = m[UI_METRIC_WPS_VOLUME_HEIGHT];
    lcd_set_foreground(wps->meter_peak);
    lcd_fillrect(x, y, width, height);
    lcd_set_foreground(wps->fg);
    y = m[UI_METRIC_WPS_TRANSPORT_Y] + layer_y + 4;
    lcd_putsxy(left + m[UI_METRIC_STATUS_PAD_LEFT], y,
               (const unsigned char *)(wps->volume_muted ? "M" : "N"));
    lcd_putsxy(left + LCD_WIDTH - m[UI_METRIC_STATUS_PAD_RIGHT] -
               m[UI_METRIC_STATUS_ICON_SIZE], y, (const unsigned char *)"O");
    lcd_set_viewport(previous);
}

static void paint_header(const struct ui_view *view,
                         const struct ui_view_frame *frame)
{
    (void)view;
    /* Volume belongs to the bar over the progress line, not the header. */
    struct native_chrome chrome = wps->chrome;
    chrome.state.volume_visible = 0;
    struct native_chrome_indicators icons;
    native_chrome_indicators(&chrome.state, &icons);
    struct viewport *previous = lcd_set_viewport(NULL);
    native_chrome_draw(&chrome, str(LANG_NOW_PLAYING), false, frame, NULL,
                       &icons, NULL);
    lcd_set_viewport(previous);
}

/* Average of four RGB565 pixels. */
static fb_data mix4(fb_data a, fb_data b, fb_data c, fb_data d)
{
    unsigned rb = (a & 0xf81f) + (b & 0xf81f) + (c & 0xf81f) + (d & 0xf81f);
    unsigned g = (a & 0x07e0) + (b & 0x07e0) + (c & 0x07e0) + (d & 0x07e0);
    return ((rb >> 2) & 0xf81f) | ((g >> 2) & 0x07e0);
}

/* a over b, with a weighing `w` of 32. */
static fb_data blend32(fb_data a, fb_data b, unsigned w)
{
    unsigned rb = ((a & 0xf81f) * w + (b & 0xf81f) * (32 - w)) >> 5;
    unsigned g = ((a & 0x07e0) * w + (b & 0x07e0) * (32 - w)) >> 5;
    return (rb & 0xf81f) | (g & 0x07e0);
}

/* The cover's pixels, or with none a drawn placeholder: a pair of beamed
 * notes on a 96-unit square. */
struct art_source {
    const fb_data *pixels;
    int stride, side;
    fb_data paper, ink;
};

static fb_data art_pixel(const struct art_source *src, int u, int v)
{
    if (src->pixels)
        return src->pixels[v * src->stride + u];
    static const struct { int8_t x, y; } heads[] = { { 37, 66 }, { 58, 60 } };
    for (size_t i = 0; i < ARRAYLEN(heads); i++)
    {
        int dx = u - heads[i].x, dy = v - heads[i].y;
        if (dx * dx * 36 + dy * dy * 81 <= 36 * 81)
            return src->ink;
    }
    int beam = 30 - (u - 42) * 6 / 23;
    bool stem = (u >= 42 && u < 45 && v >= 30 && v < 66) ||
                (u >= 62 && u < 65 && v >= 24 && v < 60);
    bool bar = u >= 42 && u < 65 && v >= beam && v < beam + 7;
    return stem || bar ? src->ink : src->paper;
}

/* One screen column of the turned cover, in 1/256 px. */
struct art_column {
    int top, height, inverse;
    uint16_t u[2];
};

static void art_columns(struct art_column *col, int first, int last, int side)
{
    const int w = CLASSIC_ART_WIDTH, near = CLASSIC_ART_NEAR;
    const int far = CLASSIC_ART_FAR;
    for (int c = first; c < last; ++c)
    {
        int t = (2 * c + 1) * 128 / w;
        col[c].top = CLASSIC_ART_DROP * t;
        col[c].height = near * 256 + (far - near) * t;
        col[c].inverse = (side << 16) / col[c].height;
        /* Perspective: texture position is linear in depth, not in x. */
        for (int k = 0; k < 2; ++k)
        {
            int s = 4 * c + 1 + 2 * k;
            col[c].u[k] = side * s * far / ((4 * w - s) * near + s * far);
        }
    }
}

/* The source row under screen position y (in 1/256 px) of a column. */
static int art_row(const struct art_column *col, int y, int side)
{
    int v = (y - col->top) * col->inverse >> 16;
    return v < 0 ? 0 : v >= side ? side - 1 : v;
}

static fb_data art_sample(const struct art_source *src,
                          const struct art_column *col, int y)
{
    int a = art_row(col, y + 64, src->side);
    int b = art_row(col, y + 192, src->side);
    return mix4(art_pixel(src, col->u[0], a), art_pixel(src, col->u[1], a),
                art_pixel(src, col->u[0], b), art_pixel(src, col->u[1], b));
}

static int overlap(int y, int from, int to)
{
    return MAX(0, MIN(y + 256, to) - MAX(y, from));
}

/* Classic art: the turned cover and, with `reflect`, its reflection, edges
 * antialiased over `under`. With no cover of its own the carousel's landing
 * stands in, and the result is false. */
static bool paint_turned(const struct ui_view_frame *frame, fb_data under,
                         bool reflect)
{
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame,
                                               FONT_SYSFIXED);
    struct art_source src = { .side = 96, .ink = wps->meter_peak };
    uint32_t rgb = ui_theme_color(&wps->theme,
                                  UI_COLOR_HANDOFF_COVER_PLACEHOLDER, NULL);
    src.paper = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
    struct bitmap *art = NULL;
    bool pinned = wps->art_handle >= 0 && wps_art_pin(wps->art_handle, true);
    if (pinned && wps_art_bitmap(wps->art_handle, &art) && art->data &&
        art->width > 0 && art->height > 0)
    {
        /* Square centre crop. */
        src.side = MIN(art->width, art->height);
        src.stride = art->width;
        src.pixels = (const fb_data *)art->data +
            (art->height - src.side) / 2 * art->width +
            (art->width - src.side) / 2;
    }
    else if (wps->landing)
    {
        if (pinned)
            wps_art_pin(wps->art_handle, false);
        lcd_set_foreground(under);
        lcd_fillrect(0, 0, viewport.width, viewport.height);
        lcd_set_viewport(previous);
        wps->landing(&frame->clip);
        return false;
    }

    int first = MAX(0, frame->clip.x - frame->bounds.x);
    int last = MIN(CLASSIC_ART_WIDTH,
                   frame->clip.x + frame->clip.width - frame->bounds.x);
    int top = frame->clip.y - frame->bounds.y;
    struct art_column column[CLASSIC_ART_WIDTH];
    fb_data row[CLASSIC_ART_WIDTH];
    art_columns(column, first, last, src.side);
    for (int dy = top; dy < top + frame->clip.height; ++dy)
    {
        int y = dy * 256;
        for (int c = first; c < last; ++c)
        {
            const struct art_column *col = &column[c];
            int bottom = col->top + col->height;
            /* Mirror about the blank row's middle. */
            int start = bottom + 256, end = start + CLASSIC_REFLECTION * 256;
            fb_data pixel = under;
            int cover = reflect ? overlap(y, start, end) : 0;
            if (cover)
            {
                int fade = CLASSIC_REFLECTION_ALPHA * (end - y - 128) /
                           (end - start);
                fb_data mirror = art_sample(&src, col,
                                            2 * (bottom + 128) - y - 256);
                pixel = blend32(mirror, pixel, fade * cover >> 8);
            }
            cover = overlap(y, col->top, bottom);
            if (cover)
                pixel = blend32(art_sample(&src, col, y), pixel, cover >> 3);
            row[c] = pixel;
        }
        lcd_bitmap(row + first, frame->bounds.x - frame->clip.x + first,
                   dy - top, last - first, 1);
    }
    if (pinned)
        wps_art_pin(wps->art_handle, false);
    lcd_set_viewport(previous);
    return src.pixels != NULL;
}

static void paint_art(const struct ui_view *view,
                      const struct ui_view_frame *frame)
{
    (void)view;
    bool own = paint_turned(frame, wps->bg, true);
    painted_art = own || wps->landing;
    if (own)
        wps->landing = NULL;
}

/* A glossy bar, one gradient row per scanline, over a blank row and a
 * reflection of itself that fades out. */
static void paint_bar(const struct ui_view_frame *frame,
                      const struct ui_theme_gradient *gradient,
                      uint32_t fallback)
{
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame,
                                               FONT_SYSFIXED);
    int offset = frame->clip.y - frame->bounds.y;
    for (int y = 0; y < viewport.height; ++y)
    {
        int row = offset + y, reflect = row - CLASSIC_BAR_HEIGHT - 1;
        if (reflect >= 0)
            row = CLASSIC_BAR_HEIGHT - 1 - reflect;
        else if (row >= CLASSIC_BAR_HEIGHT)
            continue;
        uint32_t rgb = ui_theme_gradient_at(gradient, row,
                                            CLASSIC_BAR_HEIGHT, fallback);
        lcd_set_foreground(LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255,
                                       rgb & 255));
        if (reflect < 0)
            lcd_hline(0, viewport.width - 1, y);
        else
        {
            const int rows = CLASSIC_BAR_REFLECTION;
            lcd_blendrect(0, y, viewport.width, 1,
                          (5 * (rows - reflect) + rows - 1) / rows);
        }
    }
    lcd_set_viewport(previous);
}

/* Stock's track is its own bar from where the fill ends: a tile repeated
 * from there, the last one against the right end. The fill's tile repeats
 * from the left. Both carry their reflection. */
static void paint_track(const struct ui_view *view,
                        const struct ui_view_frame *frame)
{
    if (ui_theme_art(&wps->theme))
    {
        /* The progress and volume tracks each hold their fill. */
        const struct ui_view *fill = view->first_child;
        int filled = fill ? MIN(MAX(fill->value[UI_WIDTH], 0),
                                frame->bounds.width) : 0;
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_BACKGROUND,
                    frame->bounds.x + filled, frame->bounds.y,
                    frame->bounds.width - filled, true, frame->clip);
        return;
    }
    paint_bar(frame, &wps->theme.gradient[UI_GRADIENT_PROGRESS_TRACK],
              wps->muted_rgb);
}

static void paint_fill(const struct ui_view *view,
                       const struct ui_view_frame *frame)
{
    (void)view;
    if (ui_theme_art(&wps->theme))
    {
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_FILL,
                    frame->bounds.x, frame->bounds.y, frame->bounds.width,
                    false, frame->clip);
        return;
    }
    paint_bar(frame, &wps->theme.gradient[UI_GRADIENT_PROGRESS],
              wps->accent_rgb);
}

/* A speaker facing right, its cone `y` centred, with `waves` sound arcs. */
static void speaker(int x, int y, int waves)
{
    lcd_fillrect(x, y - 2, 6, 5);
    lcd_vline(x + 5, y - 3, y + 3);
    lcd_vline(x + 6, y - 5, y + 5);
    lcd_vline(x + 7, y - 6, y + 6);
    for (int wave = 0; wave < waves; ++wave)
    {
        int r = 3 + 5 * wave;
        for (int dy = -r * 4 / 5; dy <= r * 4 / 5; ++dy)
        {
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
                ++dx;
            lcd_drawpixel(x + 7 + dx, y + dy);
        }
    }
}

/* Two crossing arrows, pointing right. */
static void paint_shuffle(const struct ui_view *view,
                          const struct ui_view_frame *frame)
{
    (void)view;
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame,
                                               FONT_SYSFIXED);
    int x = frame->bounds.x - frame->clip.x;
    int y = frame->bounds.y - frame->clip.y;
    if (ui_theme_art(&wps->theme))
    {
        stock_art_draw(STOCK_ART_NOWPLAYING_CLASSIC_SHUFFLE, frame->bounds.x,
                       frame->bounds.y, frame->clip);
        lcd_set_viewport(previous);
        return;
    }
    lcd_set_foreground(wps->fg);
    for (int arrow = 0; arrow < 2; ++arrow)
    {
        /* Rows 3 and 10 of the 18 x 14 icon, swapped for the second. */
        int from = y + (arrow ? 10 : 3), to = y + (arrow ? 3 : 10);
        lcd_hline(x, x + 4, from);
        lcd_drawline(x + 4, from, x + 10, to);
        lcd_drawline(x + 5, from, x + 11, to);
        lcd_hline(x + 10, x + 16, to);
        lcd_drawline(x + 14, to - 2, x + 16, to);
        lcd_drawline(x + 14, to + 2, x + 16, to);
    }
    lcd_set_viewport(previous);
}

static void paint_classic_volume(const struct ui_view *view,
                                 const struct ui_view_frame *frame)
{
    struct viewport viewport;
    struct viewport *previous = paint_viewport(&viewport, frame,
                                               FONT_SYSFIXED);
    int x = frame->bounds.x - frame->clip.x;
    int y = frame->bounds.y - frame->clip.y + CLASSIC_SPEAKER_Y;
    lcd_set_foreground(wps->fg);
    if (ui_theme_art(&wps->theme))
    {
        /* Stock's speakers stand level with the bar's top. */
        y = frame->bounds.y + CLASSIC_BAR_Y;
        stock_art_draw(STOCK_ART_NOWPLAYING_WHITE_VOLUME_LOW,
                       frame->bounds.x + CLASSIC_QUIET_X, y, frame->clip);
        stock_art_draw(STOCK_ART_NOWPLAYING_WHITE_VOLUME_HIGH,
                       frame->bounds.x + CLASSIC_LOUD_X, y, frame->clip);
    }
    else
    {
        speaker(x + CLASSIC_QUIET_X, y, wps->volume_muted ? 0 : 1);
        speaker(x + CLASSIC_LOUD_X, y, 3);
    }
    lcd_set_viewport(previous);

    struct ui_view_frame track = *frame;
    track.bounds = (struct ui_rect){ frame->bounds.x + CLASSIC_VOLUME_X,
        frame->bounds.y + CLASSIC_BAR_Y, wps->volume_span, CLASSIC_BAR_VIEW };
    int x0 = MAX(track.bounds.x, frame->clip.x);
    int y0 = MAX(track.bounds.y, frame->clip.y);
    int x1 = MIN(track.bounds.x + track.bounds.width,
                 frame->clip.x + frame->clip.width);
    int y1 = MIN(track.bounds.y + track.bounds.height,
                 frame->clip.y + frame->clip.height);
    if (x1 > x0 && y1 > y0)
    {
        track.clip = (struct ui_rect){ x0, y0, x1 - x0, y1 - y0 };
        paint_track(view, &track);
    }
}

static const struct ui_view_ops background_ops = { .paint = paint_background };
static const struct ui_view_ops cover_ops = { .paint = paint_cover };
static const struct ui_view_ops panel_ops = { .paint = paint_panel };
static const struct ui_view_ops label_ops = { .paint = paint_label };
static const struct ui_view_ops progress_ops = { .paint = paint_progress };
static const struct ui_view_ops progress_fill_ops = { .paint = paint_progress_fill };
static const struct ui_view_ops meter_ops = { .paint = paint_meter };
static const struct ui_view_ops volume_ops = { .paint = paint_volume };
static const struct ui_view_ops header_ops = { .paint = paint_header };
static const struct ui_view_ops art_ops = { .paint = paint_art };
static const struct ui_view_ops track_ops = { .paint = paint_track };
static const struct ui_view_ops fill_ops = { .paint = paint_fill };
static const struct ui_view_ops shuffle_ops = { .paint = paint_shuffle };
static const struct ui_view_ops classic_volume_ops = {
    .paint = paint_classic_volume
};

/* Screen bounds of a view, without its own offset. */
static struct ui_rect view_bounds(enum wps_view id)
{
    const struct ui_view *view = &wps->view[id];
    const int *v = view->value;
    struct ui_rect bounds = { v[UI_X], v[UI_Y], v[UI_WIDTH], v[UI_HEIGHT] };
    for (view = view->parent; view; view = view->parent)
    {
        bounds.x += view->value[UI_X] + view->value[UI_OFFSET_X];
        bounds.y += view->value[UI_Y] + view->value[UI_OFFSET_Y];
    }
    return bounds;
}

static struct ui_rect theme_cover_bounds(const struct ui_theme *theme)
{
    int side = MIN(theme->metric[UI_METRIC_COVER_SIZE],
                   MIN(LCD_WIDTH, LCD_HEIGHT));
    return (struct ui_rect){LCD_WIDTH - side, (LCD_HEIGHT - side) / 2,
                            side, side};
}

/* The face of the turned cover, without its reflection. */
static const struct ui_rect turned_face = {
    CLASSIC_ART_X, CLASSIC_ART_Y, CLASSIC_ART_WIDTH, CLASSIC_ART_NEAR
};

bool native_wps_art_turned(void)
{
    return active ? wps->classic :
        ui_theme_style(UI_STYLE_WPS_LAYOUT) == UI_WPS_LAYOUT_CLASSIC;
}

/* Scrim's square or the turned cover. Light has neither. */
static bool has_art_slot(void)
{
    return wps->classic ||
           wps->theme.background[UI_BG_WPS] == UI_BG_COVER_SCRIM;
}

bool native_wps_art_layout(struct ui_rect *bounds)
{
    if (active)
    {
        if (wps->classic)
        {
            *bounds = turned_face;
            return true;
        }
        if (wps->theme.background[UI_BG_WPS] != UI_BG_COVER_SCRIM)
        {
            *bounds = (struct ui_rect){0};
            return true;
        }
        *bounds = view_bounds(WPS_COVER);
        if (bounds->width > 0 && bounds->height > 0)
            return true;
        *bounds = theme_cover_bounds(&wps->theme);
    }
    else
    {
        struct ui_theme theme;
        ui_theme_snapshot(&theme);
        *bounds = theme.style[UI_STYLE_WPS_LAYOUT] == UI_WPS_LAYOUT_CLASSIC ?
                  turned_face :
                  theme.background[UI_BG_WPS] == UI_BG_COVER_SCRIM ?
                  theme_cover_bounds(&theme) : (struct ui_rect){0};
    }
    return false;
}

int native_wps_art_side(void)
{
    struct bitmap *art;
    int side = 0;
    if (!active || wps->art_handle < 0 || !wps_art_pin(wps->art_handle, true))
        return 0;
    if (wps_art_bitmap(wps->art_handle, &art) && art->data)
        side = MIN(art->width, art->height);
    wps_art_pin(wps->art_handle, false);
    return MAX(side, 0);
}

void native_wps_copy_art(fb_data *dst, int side)
{
    struct bitmap *art;
    if (!active || wps->art_handle < 0 || !wps_art_pin(wps->art_handle, true))
        return;
    if (wps_art_bitmap(wps->art_handle, &art) && art->data &&
        MIN(art->width, art->height) == side)
    {
        const fb_data *src = (const fb_data *)art->data +
            (art->height - side) / 2 * art->width + (art->width - side) / 2;
        for (int x = 0; x < side; x++)
            for (int y = 0; y < side; y++)
                dst[x * side + y] = src[y * art->width + x];
    }
    wps_art_pin(wps->art_handle, false);
}

/* The page slides under Scrim's stationary artwork and its controls pass
 * over it. Light's opaque page covers the original scene from the left. The
 * turned cover stays where it is too; the page brings its reflection, and
 * the landing draws the part of the cover the page has not reached. */
void native_wps_paint_slide(int hidden, fb_data base)
{
    if (!active || rock_ui_raw())
        return;
    struct ui_view *root = &wps->view[WPS_ROOT];
    struct ui_view *cover = &wps->view[WPS_COVER];
    const struct ui_view_ops *cover_paint = cover->ops;
    struct ui_rect screen = view_bounds(WPS_ROOT);
    hidden = MIN(MAX(hidden, 0), LCD_WIDTH);
    if (!hidden)
    {
        /* The turned cover keeps a return landing past the whole page. */
        void (*landing)(const struct ui_rect *clip) = wps->landing;
        ui_page_invalidate(&wps->page, screen);
        ui_page_paint(&wps->page);
        if (wps->classic)
            wps->landing = landing;
        return;
    }

    int width = LCD_WIDTH - hidden;
    struct viewport *previous = lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(base);
    lcd_fillrect(width, 0, hidden, LCD_HEIGHT);
    if (wps->landing && !has_art_slot())
    {
        const struct ui_rect exposed = {width, 0, hidden, LCD_HEIGHT};
        wps->landing(&exposed);
    }
    if (width && wps->background > 0)
    {
        fb_data *src = core_get_data_pinned(wps->background);
        lcd_bitmap_part(src, hidden, 0, LCD_WIDTH, 0, 0, width, LCD_HEIGHT);
        core_put_data_pinned(src);
        reveal_background(width, LCD_HEIGHT);
    }
    else if (width)
    {
        lcd_set_foreground(wps->bg);
        lcd_fillrect(0, 0, width, LCD_HEIGHT);
    }
    lcd_set_viewport(previous);

    if (wps->classic)
    {
        struct ui_rect slot = view_bounds(WPS_COVER);
        int edge = MIN(MAX(width, slot.x), slot.x + slot.width);
        struct ui_view_frame frame = { .bounds = slot, .clip = slot };
        frame.clip.width = edge - slot.x;
        if (frame.clip.width)
            paint_turned(&frame, wps->bg, true);
        const struct ui_rect exposed = {width, 0, hidden, LCD_HEIGHT};
        frame.clip.x = edge;
        frame.clip.width = slot.x + slot.width - edge;
        if (wps->landing)
            wps->landing(&exposed);
        else if (frame.clip.width)
            paint_turned(&frame, base, false);
    }
    else if (wps->theme.background[UI_BG_WPS] == UI_BG_COVER_SCRIM)
    {
        struct ui_rect slot = view_bounds(WPS_COVER);
        struct ui_view_frame frame = { .bounds = slot, .clip = slot };
        if (!width && wps->landing)
            paint_landing(&frame);
        else
            paint_cover(cover, &frame);
    }
    if (!width)
        return;

    /* Views parked to the right of the page stay off the exposed scene. */
    root->ops = NULL;
    cover->ops = NULL;
    root->value[UI_OFFSET_X] -= hidden;
    root->value[UI_CLIP_X] = hidden;
    root->value[UI_CLIP_WIDTH] = width;
    ui_page_invalidate(&wps->page, screen);
    ui_page_paint(&wps->page);
    root->value[UI_CLIP_WIDTH] = -1;
    root->value[UI_CLIP_X] = 0;
    root->value[UI_OFFSET_X] += hidden;
    cover->ops = cover_paint;
    root->ops = &background_ops;
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
    if (global_settings.scrolling_enabled && label->scroll && label->width > bounds.width)
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
    else if (label->offset)
    {
        label->offset = 0;
        ui_page_invalidate(&wps->page, bounds);
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
    if (!same_track && track && *track->path)
        strlcpy(progress->path, track->path, sizeof(progress->path));

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
    int track_width = wps->progress_span;
    /* Scale before multiplication so this also works on 32-bit targets. */
    while (length > (unsigned long)INT_MAX / track_width)
    {
        length >>= 1;
        position >>= 1;
    }
    /* Stock shows no fill at 0:00. */
    int width = length ? MAX(!wps->classic,
                             (int)(position * track_width / length)) : 0;
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

static void refresh_labels(const struct wps_state *state, bool remote, uint32_t now)
{
    if (wps->labels_frozen)
    {
        uint32_t held = now - wps->progress.clock;
        wps->title.scroll_started += held;
        wps->artist.scroll_started += held;
        wps->labels_frozen = false;
    }
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

    int index = remote ? 1 : MAX(0, playlist_get_display_index());
    if (!wps->classic)
        snprintf(buf, sizeof(buf), "%02d", index);
    else if (remote)
        buf[0] = '\0';
    else
        snprintf(buf, sizeof(buf), "%d %s %d", index, str(LANG_OF),
                 playlist_amount());
    label_update(WPS_INDEX, &wps->index, buf, now);

    unsigned long length = track ? track->length : 0;
    /* Preview offsets are signed. Clamp both ends before deriving labels. */
    int64_t preview = track ? (int64_t)track->elapsed + state->ff_rewind_count : 0;
    unsigned long position = preview < 0 ? 0 : MIN((uint64_t)preview, length);
    char *time = buf;
    if (wps->classic)
    {
        label_update(WPS_ALBUM, &wps->album, track ? track->album : NULL, now);
        format_time(buf, sizeof(buf), (long)MIN(position, LONG_MAX));
        label_update(WPS_ELAPSED, &wps->elapsed, buf, now);
        *time++ = '-';
    }
    format_time(time, sizeof(buf) - (time - buf),
                (long)MIN(length - position, LONG_MAX));
    label_update(WPS_REMAINING, &wps->remaining, buf, now);

    refresh_progress(state, remote, now, position, current_playmode());
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

static int handoff_art(void)
{
#ifdef HAVE_USB_PODLINK
    if (cider_active())
    {
        if (!*wps->cover_remote_key)
            cider_current_art(wps->cover_remote_key);
        if (!*wps->cover_remote_key ||
            strcmp(wps->cover_remote_key, cider_wps_art_key()))
            return -1;
        return album_art_current();
    }
#endif
    return playback_track_aa_hid(art_slot);
}

static void refresh_art(void)
{
    album_palette_refresh();
    unsigned palette_generation = album_palette_generation();
    unsigned art_generation = album_art_generation();
    int handle = album_art_current();
    if (wps->current_art_only || (wps->landing && has_art_slot()))
    {
        /* An ordinary WPS retains the last cover during a track change.
         * A flip belongs to the newly selected track, including missing art. */
        handle = handoff_art();
        if (handle >= 0 || handle == ERR_UNSUPPORTED_TYPE)
            wps->current_art_only = false;
    }
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
        wps->accent_rgb = ui_theme_color(&wps->theme, UI_COLOR_WPS_ACCENT,
                                         &palette);
        wps->muted_rgb = ui_theme_color(&wps->theme, UI_COLOR_WPS_MUTED,
                                        &palette);
    }
    unsigned blur_generation = album_blur_generation();
    if (wps->art_generation != art_generation || wps->art_handle != handle || colors_changed ||
        wps->blur_generation != blur_generation)
    {
        wps->art_generation = art_generation;
        wps->blur_generation = blur_generation;
        wps->art_handle = handle;
        int bg = native_wps_background(handle, art_generation,
                            wps->theme.background[UI_BG_WPS], wps->fg);
        if (wps->wait_for_blur && bg > 0)
        {
            wps->wait_for_blur = false;
            struct ui_view *root = &wps->view[WPS_ROOT];
            ui_page_set(&wps->page, root, UI_BRIGHTNESS, 0);
            ui_page_tween(&wps->page, &wps->blur_reveal, root, UI_BRIGHTNESS,
                          UI_FULL_OPACITY, ANIM_F(12), false, USEC_TIMER);
        }
        if (bg > 0 || !wps->cover_backdrop)
        {
            wps->background = bg;
            wps->cover_backdrop = false;
        }
        ui_page_invalidate(&wps->page, view_bounds(WPS_ROOT));
    }
}

/* Picking up a texture must not change the moving controls or their ink. */
void native_wps_pick_up_art(void)
{
    if (!active || rock_ui_raw())
        return;
    int art = handoff_art();
    if (art >= 0)
    {
        wps->art_handle = art;
        wps->art_generation = album_art_generation();
    }
}

static void refresh_volume(const struct cider_status *source, bool remote, uint32_t now)
{
    bool visible = volume_changed_within(now, ui_frame_clock(
        wps->classic ? CLASSIC_VOLUME_HOLD : VOLUME_HOLD_FRAMES));
    int min = sound_min(SOUND_VOLUME);
    int max = sound_max(SOUND_VOLUME);
    int percent = remote ? source->volume :
        (global_status.volume - min) * 100 / MAX(1, max - min);
    percent = MIN(100, MAX(0, percent));
    bool muted = remote ? source->muted : global_status.volume <= min;
    int target = wps->volume_span * percent / 100;
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
    if (wps->volume_visible != visible && wps->classic)
    {
        /* The incoming row enters from the right as the other leaves left. */
        struct ui_view *row = &wps->view[WPS_ROW];
        struct ui_view *timeline = &wps->view[WPS_TIMELINE];
        ui_page_set(&wps->page, visible ? timeline : overlay, UI_X, 0);
        ui_page_set(&wps->page, visible ? overlay : timeline, UI_X, LCD_WIDTH);
        ui_page_set(&wps->page, row, UI_OFFSET_X, 0);
        ui_page_tween(&wps->page, &wps->volume_reveal, row, UI_OFFSET_X,
                      -LCD_WIDTH, CLASSIC_SLIDE_FRAMES, false, now);
        wps->volume_reveal.tween.easing = UI_EASE_QUAD;
        wps->volume_visible = visible;
    }
    else if (wps->volume_visible != visible)
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
    wps->peak_height = m[UI_METRIC_WPS_METER_PEAK_HEIGHT];
    const struct ui_rect bounds[WPS_VIEW_COUNT] = {
        [WPS_ROOT] = { 0, 0, LCD_WIDTH, LCD_HEIGHT },
        [WPS_COVER] = theme_cover_bounds(&wps->theme),
        [WPS_PANEL] = { 0, m[UI_METRIC_WPS_PANEL_TOP], m[UI_METRIC_WPS_PROGRESS_WIDTH],
                        MAX(0, m[UI_METRIC_WPS_PROGRESS_Y] - m[UI_METRIC_WPS_PANEL_TOP]) },
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
    wps->progress_span = bounds[WPS_PROGRESS].width;
    wps->volume_span = m[UI_METRIC_WPS_VOLUME_WIDTH];
    for (int i = WPS_ROOT + 1; i <= WPS_VOLUME_FILL; i++)
    {
        enum wps_view parent = i == WPS_PROGRESS_FILL ? WPS_PROGRESS :
                               i == WPS_VOLUME_FILL ? WPS_VOLUME : WPS_ROOT;
        ui_view_append(&wps->view[parent], &wps->view[i]);
    }
    ui_page_init(&wps->page, &wps->view[WPS_ROOT]);
}

static void setup_classic(void)
{
    enum { ROW = CLASSIC_ROW_HEIGHT, TIME_W = CLASSIC_PROGRESS_X,
           REMAINING_X = CLASSIC_REMAINING_RIGHT - TIME_W };
    static const struct {
        uint8_t id, parent;
        struct ui_rect bounds;
    } layout[] = {
        { WPS_HEADER, WPS_ROOT, { 0, 0, LCD_WIDTH, 0 } },
        { WPS_COVER, WPS_ROOT, { CLASSIC_ART_X, CLASSIC_ART_Y,
                                 CLASSIC_ART_WIDTH,
                                 CLASSIC_ART_NEAR + 1 + CLASSIC_REFLECTION } },
        { WPS_TITLE, WPS_ROOT, { CLASSIC_TEXT_X, CLASSIC_TITLE_Y,
                                 CLASSIC_TEXT_WIDTH, CLASSIC_TITLE_HEIGHT } },
        { WPS_ARTIST, WPS_ROOT, { CLASSIC_TEXT_X, CLASSIC_ARTIST_Y,
                                  CLASSIC_TEXT_WIDTH, CLASSIC_LINE } },
        { WPS_ALBUM, WPS_ROOT, { CLASSIC_TEXT_X, CLASSIC_ALBUM_Y,
                                 CLASSIC_TEXT_WIDTH, CLASSIC_LINE } },
        { WPS_INDEX, WPS_ROOT, { CLASSIC_TEXT_X, CLASSIC_INDEX_Y,
                                 CLASSIC_TEXT_WIDTH, CLASSIC_LINE } },
        { WPS_SHUFFLE, WPS_ROOT, { CLASSIC_SHUFFLE_X, CLASSIC_SHUFFLE_Y,
                                   18, 14 } },
        { WPS_ROW, WPS_ROOT, { 0, CLASSIC_ROW_Y, 2 * LCD_WIDTH, ROW } },
        { WPS_TIMELINE, WPS_ROW, { 0, 0, LCD_WIDTH, ROW } },
        { WPS_PROGRESS, WPS_TIMELINE, { CLASSIC_PROGRESS_X, CLASSIC_BAR_Y,
                                        CLASSIC_PROGRESS_WIDTH,
                                        CLASSIC_BAR_VIEW } },
        { WPS_PROGRESS_FILL, WPS_PROGRESS, { 0, 0, 0, CLASSIC_BAR_VIEW } },
        { WPS_ELAPSED, WPS_TIMELINE, { CLASSIC_ELAPSED_X, CLASSIC_TIME_Y,
                                       TIME_W - CLASSIC_ELAPSED_X,
                                       CLASSIC_LINE } },
        { WPS_REMAINING, WPS_TIMELINE, { REMAINING_X, CLASSIC_TIME_Y,
                                         TIME_W, CLASSIC_LINE } },
        { WPS_VOLUME, WPS_ROW, { LCD_WIDTH, 0, LCD_WIDTH, ROW } },
        { WPS_VOLUME_FILL, WPS_VOLUME, { CLASSIC_VOLUME_X, CLASSIC_BAR_Y,
                                         0, CLASSIC_BAR_VIEW } },
    };
    static const struct ui_view_ops *const ops[WPS_VIEW_COUNT] = {
        [WPS_HEADER] = &header_ops, [WPS_COVER] = &art_ops,
        [WPS_INDEX] = &label_ops, [WPS_TITLE] = &label_ops,
        [WPS_ARTIST] = &label_ops, [WPS_ALBUM] = &label_ops,
        [WPS_PROGRESS] = &track_ops, [WPS_PROGRESS_FILL] = &fill_ops,
        [WPS_ELAPSED] = &label_ops, [WPS_REMAINING] = &label_ops,
        [WPS_VOLUME] = &classic_volume_ops, [WPS_VOLUME_FILL] = &fill_ops,
        [WPS_SHUFFLE] = &shuffle_ops,
    };
    void *const data[WPS_VIEW_COUNT] = {
        [WPS_INDEX] = &wps->index, [WPS_TITLE] = &wps->title,
        [WPS_ARTIST] = &wps->artist, [WPS_ALBUM] = &wps->album,
        [WPS_ELAPSED] = &wps->elapsed, [WPS_REMAINING] = &wps->remaining,
    };

    ui_view_init(&wps->view[WPS_ROOT],
                 (struct ui_rect){ 0, 0, LCD_WIDTH, LCD_HEIGHT },
                 &background_ops, NULL);
    for (size_t i = 0; i < ARRAYLEN(layout); i++)
    {
        struct ui_view *view = &wps->view[layout[i].id];
        ui_view_init(view, layout[i].bounds, ops[layout[i].id],
                     data[layout[i].id]);
        ui_view_append(&wps->view[layout[i].parent], view);
    }
    wps->view[WPS_HEADER].value[UI_HEIGHT] =
        wps->theme.metric[UI_METRIC_STATUS_HEIGHT];
    wps->artist.muted = wps->album.muted = wps->index.muted = true;
    wps->view[WPS_SHUFFLE].value[UI_OPACITY] = 0;
    wps->progress_span = CLASSIC_PROGRESS_WIDTH;
    wps->volume_span = CLASSIC_VOLUME_WIDTH;
    ui_page_init(&wps->page, &wps->view[WPS_ROOT]);
}

void native_wps_open(const struct wps_state *state)
{
    (void)state;
    if (wps_handle <= 0 || active)
        return;
    painted_art = false;
    wps = core_get_data_pinned(wps_handle);
    memset(wps, 0, sizeof(*wps));
    ui_theme_snapshot(&wps->theme);
    wps->art_handle = -1;
    wps->art_generation = wps->palette_generation = (unsigned)-1;
    wps->meter_due = current_tick;

    wps->classic =
        wps->theme.style[UI_STYLE_WPS_LAYOUT] == UI_WPS_LAYOUT_CLASSIC;
    label_set(&wps->title, ui_theme_font_load(&wps->theme, UI_FONT_WPS_TITLE, 48),
              wps->title_text, sizeof(wps->title_text));
    label_set(&wps->artist, ui_theme_font_load(&wps->theme,
                  wps->classic ? UI_FONT_WPS_META : UI_FONT_BODY, 48),
              wps->artist_text, sizeof(wps->artist_text));
    label_set(&wps->index, ui_theme_font_load(&wps->theme, UI_FONT_WPS_META, 48),
              wps->index_text, sizeof(wps->index_text));
    label_set(&wps->remaining, wps->index.font, wps->remaining_text, sizeof(wps->remaining_text));
    wps->volume_font = wps->classic ? -1 :
                       ui_theme_font_load(&wps->theme, UI_FONT_ICON, 48);

    wps->remaining.right = true;
    wps->title.scroll = wps->artist.scroll = true;
    if (wps->classic)
    {
        /* These share the artist and index fonts; close unloads those. */
        label_set(&wps->album, wps->artist.font, wps->album_text,
                  sizeof(wps->album_text));
        label_set(&wps->elapsed, wps->index.font, wps->elapsed_text,
                  sizeof(wps->elapsed_text));
        wps->album.scroll = true;
        wps->chrome_ready = native_chrome_open(&wps->chrome, &wps->theme);
        setup_classic();
    }
    else
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
    if (rock_ui_raw())
        return;
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
    if (wps->cover_backdrop || wps->wait_for_blur || wps->landing)
    {
        bool same = remote == wps->progress.remote;
        if (same && state->id3 && *state->id3->path && *wps->progress.path)
            same = !strcmp(wps->progress.path, state->id3->path);
#ifdef HAVE_USB_PODLINK
        if (same && remote)
        {
            char key[65];
            cider_current_art(key);
            if (*key && *wps->cover_remote_key)
                same = !strcmp(key, wps->cover_remote_key);
        }
#endif
        if (!same)
        {
            wps->landing = NULL;
            wps->current_art_only = wps->cover_backdrop = wps->wait_for_blur = false;
            wps->background = -1;
            wps->blur_generation = (unsigned)-1;
        }
    }
    refresh_art();
    uint32_t now = USEC_TIMER;
    refresh_labels(state, remote, now);
    refresh_volume(&source, remote, now);
    if (wps->classic)
    {
        bool shuffle = remote ? source.shuffle :
                       global_settings.playlist_shuffle;
        ui_page_set(&wps->page, &wps->view[WPS_SHUFFLE], UI_OPACITY,
                    shuffle ? UI_FULL_OPACITY : 0);
    }
    if (wps->chrome_ready && native_chrome_poll(&wps->chrome))
        ui_page_invalidate(&wps->page, view_bounds(WPS_HEADER));
    if (wps->page.tracks && !wps->motion_tried)
    {
        const char *route = wps->page.tracks == &wps->progress.motion ?
                            "wps-seek-progress" :
                            wps->page.tracks == &wps->blur_reveal ?
                            "wps-blur-ready" : "wps-volume";
        wps->motion_trace = ui_perf_begin(route);
        wps->motion_tried = true;
    }
    ui_page_step(&wps->page, now);
    if (!wps->classic)
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

void native_wps_paint_handoff(const struct wps_state *state,
                              const struct album_blur_source *source)
{
    if (!active || rock_ui_raw())
        return;
    strlcpy(wps->cover_remote_key, source->remote,
            sizeof(wps->cover_remote_key));
    wps->current_art_only = true;
    native_wps_render(state, false);
    wps->labels_frozen = true;
    if (wps->theme.background[UI_BG_WPS] != UI_BG_COVER_SCRIM ||
        blur_handle <= 0)
        return;

    /* The carousel and playback share the selected album's blur plane.
     * It is already usable before audio buffering publishes a WPS bitmap. */
    fb_data *pixels = core_get_data_pinned(blur_handle);
    bool ready = album_blur_render_cover_wps(pixels, source, wps->fg);
    core_put_data_pinned(pixels);
    wps->wait_for_blur = !ready && wps->background <= 0;
    wps->reveal_base = wps->bg;
    if (ready)
    {
        backdrop.art = -1;
        backdrop.ready = false;
        wps->background = blur_handle;
        wps->cover_backdrop = true;
        ui_page_invalidate(&wps->page, view_bounds(WPS_ROOT));
        ui_page_paint(&wps->page);
    }
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
    wps->landing = NULL;
    ui_perf_end(wps->motion_trace);
    ui_page_cancel_tracks(&wps->page);
    if (wps->title.font >= 0)
        font_unload(wps->title.font);
    if (wps->artist.font >= 0)
        font_unload(wps->artist.font);
    if (wps->index.font >= 0)
        font_unload(wps->index.font);
    if (wps->volume_font >= 0)
        font_unload(wps->volume_font);
    if (wps->chrome_ready)
        native_chrome_close(&wps->chrome);
    core_put_data_pinned(wps);
    wps = NULL;
}
