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

#define BAR_X 54
#define BAR_Y (LYRICS_CHROME_STRIP + 4)
#define BAR_WIDTH 212
#define BAR_HEIGHT 14
#define TIME_Y (LYRICS_CHROME_STRIP + 5)
#define ELAPSED_X 15
#define REMAINING_RIGHT 310

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
        font_prefetch_begin();
        font_prefetch_text(chrome->time_font, "0123456789:-");
        font_prefetch_end();
    }
    chrome->time_fg = pack(t->color[UI_COLOR_WPS_FG]);
    chrome->band_fallback = pack(t->color[UI_COLOR_SELECTION]);
    chrome->elapsed[0] = chrome->remaining[0] = '\0';
    /* The gradient spans the page below the status bar. */
    int top = chrome->status_height, height = LCD_HEIGHT - top;
    for (int y = 0; y < LCD_HEIGHT; y++)
        chrome->rows[y] = pack(ui_theme_gradient_at(
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
    return (int)((int64_t)elapsed * BAR_WIDTH / length);
}

long lyrics_chrome_next_ms(long elapsed, long length)
{
    if (length <= 0 || elapsed >= length)
        return 500;
    elapsed = MAX(0, elapsed);
    long pixel = lyrics_chrome_fill(elapsed, length) + 1;
    long bar = ((int64_t)pixel * length + BAR_WIDTH - 1) / BAR_WIDTH - elapsed;
    long second = 1000 - elapsed % 1000;
    long left = (length - elapsed) % 1000 + 1;
    return MAX(1, MIN(bar, MIN(second, left)));
}

/* A bar of `width` from x in one gradient, without stock's reflection. */
static void plain_bar(const struct ui_theme_gradient *gradient,
                      uint32_t fallback, int x, int width, struct ui_rect clip)
{
    int left = MAX(x, clip.x), right = MIN(x + width, clip.x + clip.width);
    int top = MAX(BAR_Y, clip.y);
    int bottom = MIN(BAR_Y + BAR_HEIGHT, clip.y + clip.height);
    fb_data *frame = FBADDR(0, 0);
    for (int y = top; y < bottom && left < right; y++)
    {
        uint32_t rgb = ui_theme_gradient_at(gradient, y - BAR_Y, BAR_HEIGHT,
                                            fallback);
        memset16(frame + y * LCD_WIDTH + left, pack(rgb), right - left);
    }
}

void lyrics_chrome_strip(struct lyrics_chrome *chrome, int fill,
                         struct ui_rect clip)
{
    const struct ui_theme *t = &chrome->theme;
    fill = MAX(0, MIN(fill, BAR_WIDTH));
    struct viewport *previous = lcd_set_viewport(NULL);
    if (ui_theme_art(t) && stock_art_ready())
    {
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_BACKGROUND,
                    BAR_X + fill, BAR_Y, BAR_WIDTH - fill, true, clip);
        ui_art_tile(STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_FILL,
                    BAR_X, BAR_Y, fill, false, clip);
    }
    else
    {
        plain_bar(&t->gradient[UI_GRADIENT_PROGRESS_TRACK],
                  t->color[UI_COLOR_WPS_MUTED], BAR_X + fill,
                  BAR_WIDTH - fill, clip);
        plain_bar(&t->gradient[UI_GRADIENT_PROGRESS],
                  t->color[UI_COLOR_WPS_ACCENT], BAR_X, fill, clip);
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
        const unsigned char *left = (const unsigned char *)chrome->remaining;
        int width = font_getstringsize(left, NULL, NULL, chrome->time_font);
        lcd_putsxy(ELAPSED_X - clip.x, TIME_Y - clip.y,
                   (const unsigned char *)chrome->elapsed);
        lcd_putsxy(REMAINING_RIGHT - width - clip.x, TIME_Y - clip.y, left);
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
