/* The chrome of the lyrics page under style.lyrics=classic. GPL version 2+.
 * The geometry of the progress row is Now Playing's under
 * style.wps_layout=classic, so the bar and times stay put across the page
 * change. Everything here draws into the default framebuffer, unflushed. */
#include <string.h>
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "font.h"
#include "lang.h"
#include "system/format_time.h"
#include "ui/native_theme_runtime.h"
#include "ui/theme_art.h"
#include "lyrics_chrome.h"

#define BAR_Y (WPS_CLASSIC_PROGRESS_TOP + WPS_CLASSIC_BAR_Y)
#define TIME_Y (WPS_CLASSIC_PROGRESS_TOP + WPS_CLASSIC_TIME_Y)

static fb_data pack(uint32_t rgb)
{
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

void lyrics_chrome_open(struct lyrics_chrome *chrome,
                        const struct ui_theme *theme)
{
    chrome->theme = *theme;
    const struct ui_theme *t = &chrome->theme;
    chrome->status_height = t->metric[UI_METRIC_STATUS_HEIGHT];
    chrome->status_ready = native_chrome_open(&chrome->status, t);
    chrome->time_font = ui_theme_font_load(t, UI_FONT_WPS_TIME, 32);
    /* Frames draw from the glyph cache only; a digit seen for the first
     * time would leave its time blank for a frame. */
    if (chrome->time_font >= 0)
    {
        font_prefetch_mark_hot(chrome->time_font);
        font_getstringsize((const unsigned char *)"0123456789:-",
                           NULL, NULL, chrome->time_font);
    }
    chrome->time_fg = pack(t->color[UI_COLOR_WPS_FG]);
    chrome->band_fallback = pack(t->color[UI_COLOR_SELECTION]);
    chrome->elapsed[0] = chrome->remaining[0] = '\0';
    /* The body owns its gradient. The playback strip shares WPS's paper. */
    int top = chrome->status_height, height = LCD_HEIGHT - top;
    fb_data paper = ui_theme_rgb565(UI_COLOR_WPS_BG);
    for (int y = 0; y < LCD_HEIGHT; y++)
        chrome->rows[y] = y >= WPS_CLASSIC_PROGRESS_TOP ? paper :
            pack(ui_theme_gradient_at(
            &t->gradient[UI_GRADIENT_LYRICS], MAX(0, y - top), height,
            t->color[UI_COLOR_BACKGROUND]));
}

void lyrics_chrome_close(struct lyrics_chrome *chrome)
{
    if (chrome->status_ready)
        native_chrome_close(&chrome->status);
    if (chrome->time_font >= 0)
        font_unload(chrome->time_font);
    chrome->status_ready = false;
    chrome->time_font = -1;
}

bool lyrics_chrome_poll(struct lyrics_chrome *chrome)
{
    return chrome->status_ready && native_chrome_poll(&chrome->status);
}

void lyrics_chrome_status(struct lyrics_chrome *chrome, struct ui_rect clip)
{
    if (!chrome->status_ready)
        return;
    struct native_chrome_indicators icons;
    native_chrome_indicators(&chrome->status.state, &icons);
    const struct ui_view_frame frame = {
        .bounds = { 0, 0, LCD_WIDTH, chrome->status_height },
        .clip = clip, .opacity = 256, .brightness = 256,
    };
    struct viewport *previous = lcd_set_viewport(NULL);
    native_chrome_draw(&chrome->status, str(LANG_NOW_PLAYING), false, &frame,
                       NULL, &icons, NULL);
    lcd_set_viewport(previous);
}

bool lyrics_chrome_times(struct lyrics_chrome *chrome, long elapsed,
                         long length)
{
    char now[sizeof(chrome->elapsed)], left[sizeof(chrome->remaining)];
    elapsed = MAX(0, MIN(elapsed, length));
    format_time(now, sizeof(now), elapsed);
    left[0] = '-';
    format_time(left + 1, sizeof(left) - 1, length - elapsed);
    if (!strcmp(now, chrome->elapsed) && !strcmp(left, chrome->remaining))
        return false;
    strcpy(chrome->elapsed, now);
    strcpy(chrome->remaining, left);
    return true;
}

int lyrics_chrome_fill(long elapsed, long length)
{
    if (length <= 0)
        return 0;
    elapsed = MAX(0, MIN(elapsed, length));
    return (int)((int64_t)elapsed * WPS_CLASSIC_PROGRESS_WIDTH / length);
}

long lyrics_chrome_next_ms(long elapsed, long length)
{
    if (length <= 0 || elapsed >= length)
        return 500;
    elapsed = MAX(0, elapsed);
    long pixel = lyrics_chrome_fill(elapsed, length) + 1;
    long bar = ((int64_t)pixel * length + WPS_CLASSIC_PROGRESS_WIDTH - 1) /
               WPS_CLASSIC_PROGRESS_WIDTH - elapsed;
    long second = 1000 - elapsed % 1000;
    long left = (length - elapsed) % 1000 + 1;
    return MAX(1, MIN(bar, MIN(second, left)));
}

/* The same fallback bar and reflection that Now Playing paints. */
static void plain_bar(const struct ui_theme_gradient *gradient,
                      uint32_t fallback, int x, int width, struct ui_rect clip)
{
    int left = MAX(x, clip.x), right = MIN(x + width, clip.x + clip.width);
    int top = MAX(BAR_Y, clip.y);
    const int rows = WPS_CLASSIC_BAR_REFLECTION;
    const int height = WPS_CLASSIC_BAR_HEIGHT;
    int bottom = MIN(BAR_Y + height + 1 + rows, clip.y + clip.height);
    if (left >= right || top >= bottom)
        return;
    struct viewport vp = {
        .x = left, .y = top, .width = right - left, .height = bottom - top,
        .flags = VP_DEFAULT_FLAGS, .drawmode = DRMODE_SOLID,
        .buffer = &lcd_framebuffer_default,
    };
    struct viewport *previous = lcd_set_viewport(&vp);
    for (int y = top; y < bottom; ++y)
    {
        int row = y - BAR_Y;
        int reflect = row - height - 1;
        if (row == height)
            continue;
        if (reflect >= 0)
            row = height - 1 - reflect;
        lcd_set_foreground(pack(ui_theme_gradient_at(gradient, row, height,
                                                       fallback)));
        if (reflect < 0)
            lcd_hline(0, vp.width - 1, y - top);
        else
            lcd_blendrect(0, y - top, vp.width, 1,
                          (5 * (rows - reflect) + rows - 1) / rows);
    }
    lcd_set_viewport(previous);
}

void lyrics_chrome_strip(struct lyrics_chrome *chrome, int fill,
                         struct ui_rect clip)
{
    const struct ui_theme *t = &chrome->theme;
    fill = MAX(0, MIN(fill, WPS_CLASSIC_PROGRESS_WIDTH));
    struct viewport *previous = lcd_set_viewport(NULL);
    if (ui_theme_art(t) && stock_art_ready())
    {
        stock_art_over_layer(true);
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_BACKGROUND,
                    WPS_CLASSIC_PROGRESS_X + fill, BAR_Y,
                    WPS_CLASSIC_PROGRESS_WIDTH - fill, true, clip);
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_FILL,
                    WPS_CLASSIC_PROGRESS_X, BAR_Y, fill, false, clip);
        stock_art_over_layer(false);
    }
    else
    {
        plain_bar(&t->gradient[UI_GRADIENT_PROGRESS_TRACK],
                  t->color[UI_COLOR_WPS_MUTED], WPS_CLASSIC_PROGRESS_X,
                  WPS_CLASSIC_PROGRESS_WIDTH, clip);
        plain_bar(&t->gradient[UI_GRADIENT_PROGRESS],
                  t->color[UI_COLOR_WPS_ACCENT],
                  WPS_CLASSIC_PROGRESS_X, fill, clip);
    }
    if (chrome->time_font >= 0)
    {
        struct viewport text = {
            .x = clip.x, .y = clip.y,
            .width = clip.width, .height = clip.height,
            .flags = VP_DEFAULT_FLAGS, .font = chrome->time_font,
            .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
            .fg_pattern = chrome->time_fg,
        };
        lcd_set_viewport(&text);
        lcd_set_glyph_layer(true);
        const unsigned char *left = (const unsigned char *)chrome->remaining;
        int width = font_getstringsize(left, NULL, NULL, chrome->time_font);
        lcd_putsxy(WPS_CLASSIC_ELAPSED_X - clip.x, TIME_Y - clip.y,
                   (const unsigned char *)chrome->elapsed);
        lcd_putsxy(WPS_CLASSIC_REMAINING_RIGHT - width - clip.x,
                   TIME_Y - clip.y, left);
        lcd_set_glyph_layer(false);
    }
    lcd_set_viewport(previous);
}

void lyrics_chrome_band(const struct lyrics_chrome *chrome, int top,
                        int height, int from, int to)
{
    const struct ui_theme_gradient *gradient =
        &chrome->theme.gradient[UI_GRADIENT_SELECTION];
    /* Stock's own rows for the heights a list draws. */
    const uint32_t *rows = ui_theme_art(&chrome->theme) ?
                           ui_art_selection(height) : NULL;
    fb_data *frame = FBADDR(0, 0);
    for (int y = MAX(top, from); y < MIN(top + height, to); y++)
    {
        int row = y - top;
        fb_data colour = rows ? pack(rows[row]) : !gradient->stops ?
            chrome->band_fallback :
            pack(ui_theme_gradient_at(gradient, row, height, 0));
        memset16(frame + y * LCD_WIDTH, colour, LCD_WIDTH);
    }
}
