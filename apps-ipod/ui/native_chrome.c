/* Native status bar. Layout adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */
#include "config.h"
#include "native_chrome.h"
#include "draw/line.h"
#include "native_theme_runtime.h"
#include "retained.h"

#include <stdio.h>
#include <string.h>

#include "animation.h"
#include "audio/play_status.h"
#include "database/tagcache.h"
#include "draw/color.h"
#include "files/file_index.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "led.h"
#include "metadata/art_cache.h"
#include "powermgmt.h"
#include "settings/settings.h"
#include "sound.h"
#include "system.h"
#include "system/activity.h"
#include "timefuncs.h"

#define BUSY_HIDDEN UINT8_MAX
#define BUSY_TURN ANIM_F(58)
#define VOLUME_HOLD ANIM_F(180)

static const char *const indicator_icons[CHROME_GLYPH_COUNT] = {
    "A", "B",
    "1", "2", "3", "4", "5", "6", "7",
    "·", "¸", "¹", "º",
};
static const char *const busy_icons[] = {
    "á", "â", "ã", "ä", "å", "æ", "ç", "è",
};

void native_chrome_indicators(const struct native_chrome_state *state,
                             struct native_chrome_indicators *out)
{
    *out = (struct native_chrome_indicators){ .battery = -1 };
    switch (state->playmode)
    {
        case STATUS_PLAY:
        case STATUS_FASTFORWARD:
        case STATUS_FASTBACKWARD: out->ink[CHROME_PLAY] = UINT8_MAX; break;
        case STATUS_PAUSE: out->ink[CHROME_PAUSE] = UINT8_MAX; break;
    }
    if (state->battery >= 0)
    {
        int band = state->battery * 6 / 100;
        static const uint8_t charge_band[] = {0, 1, 1, 1, 1, 2, 3};
        int glyph = state->charging ? CHROME_CHARGE_FIRST + charge_band[band] :
                                      CHROME_BATTERY_FIRST + band;
        out->ink[glyph] = UINT8_MAX;
        if (state->numeric)
            out->battery = state->battery;
    }
}

static unsigned int busy_state(void)
{
    bool busy = (global_settings.show_disk_activity && led_read(HZ / 2))
        || (global_settings.show_background_tasks
            && (tagcache_is_busy() || art_cache_is_busy() || file_index_is_busy()))
        || (global_settings.show_working && ui_working());

    if (!busy)
        return BUSY_HIDDEN;
    unsigned frame = ui_clock_frame(USEC_TIMER) % BUSY_TURN;
    return frame * ARRAYLEN(busy_icons) / BUSY_TURN;
}

bool native_chrome_poll(struct native_chrome *chrome)
{
    const struct tm *now = get_time();
    bool clock_valid = valid_time(now);
    int battery = battery_level();
    struct native_chrome_state next = {
        .battery = MIN(MAX(battery, -1), 100),
        .hour = clock_valid ? now->tm_hour : -1,
        .minute = clock_valid ? now->tm_min : 0,
        .charging = charge_state == CHARGING || charge_state == TOPOFF,
        .numeric = global_settings.battery_display != 0,
        .clock_24h = global_settings.timeformat == 0,
        .playmode = current_playmode(),
        .busy = busy_state(),
        .volume_visible = native_chrome_volume_visible(),
    };
    if (next.volume_visible)
    {
        int minimum = sound_min(SOUND_VOLUME);
        int maximum = sound_max(SOUND_VOLUME);
        int volume = MIN(MAX(global_status.volume, minimum), maximum);
        if (maximum > minimum)
            next.volume_percent = (volume - minimum) * 100 / (maximum - minimum);
    }

    /* Every member is one byte. These samples are compared to invalidate
     * only the status bar when no list geometry or contents changed. */
    bool changed = memcmp(&chrome->state, &next, sizeof(next)) != 0;
    chrome->state = next;
    return changed;
}

bool native_chrome_open(struct native_chrome *chrome, const struct ui_theme *theme)
{
    *chrome = (struct native_chrome) {
        .theme = theme,
        .status_font = -1,
        .title_font = -1,
        .icon_font = -1,
    };
    int glyphs = global_settings.glyphs_to_cache;
    chrome->status_font = ui_theme_font_load(theme, UI_FONT_STATUS, glyphs);
    if (chrome->status_font < 0)
        return false;
    chrome->title_font = ui_theme_font_load(theme, UI_FONT_TITLE, glyphs);
    chrome->icon_font = ui_theme_font_load(theme, UI_FONT_ICON, glyphs);
    if (chrome->title_font < 0 || chrome->icon_font < 0)
    {
        native_chrome_close(chrome);
        return false;
    }
    native_chrome_poll(chrome);
    return true;
}

void native_chrome_close(struct native_chrome *chrome)
{
    font_unload(chrome->icon_font);
    font_unload(chrome->title_font);
    font_unload(chrome->status_font);
    chrome->icon_font = chrome->title_font = chrome->status_font = -1;
}

bool native_chrome_volume_visible(void)
{
    return global_status.last_volume_change &&
           TIME_BEFORE(current_tick, global_status.last_volume_change +
                       VOLUME_HOLD * HZ / UI_ANIM_FPS);
}

struct status_layout {
    int battery_x, percent_x, percent_width, play_x, busy_x;
    int clock_x, clock_width, title_width;
};

static struct status_layout status_layout(const struct native_chrome *chrome, int width,
                                          int clock_width, int percent_width)
{
    const uint16_t *metric = chrome->theme->metric;
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int gap = metric[UI_METRIC_STATUS_ICON_GAP];
    int group_gap = metric[UI_METRIC_STATUS_GROUP_GAP];
    int left = metric[UI_METRIC_STATUS_PAD_LEFT];
    int battery_x = width - metric[UI_METRIC_STATUS_PAD_RIGHT] - icon_size;
    int play_x = battery_x - gap - icon_size;
    /* Reserve a centered clock even with battery percentage enabled. */
    int percent_limit = play_x - gap - group_gap - (width + clock_width + 1) / 2;
    percent_width = MIN(percent_width, MAX(0, percent_limit));
    int percent_x = battery_x - gap - percent_width;
    if (percent_width)
        play_x = percent_x - gap - icon_size;

    int clock_inset = MAX(left + icon_size, width - play_x) + group_gap;
    clock_width = MIN(clock_width, MAX(0, width - 2 * clock_inset));
    int clock_x = (width - clock_width) / 2;
    int busy_x = MAX(left, clock_x - group_gap - icon_size);
    return (struct status_layout) {
        .battery_x = battery_x, .percent_x = percent_x, .percent_width = percent_width,
        .play_x = play_x, .busy_x = busy_x,
        .clock_x = clock_x, .clock_width = clock_width,
        .title_width = busy_x - gap - left,
    };
}

static const char *title_text(const char *title)
{
    if (!title)
        return "";
    return strcmp(title, "Rockbox") == 0 ? "AstraPod" : title;
}

/* The clock is generated as ASCII in left-to-right order. Keep the full
 * PBF1 advance, including kerning, until after centering the pen. */
static int clock_advance(const char *text, int font)
{
    struct font_run run = {0};
    font_lock(font, true);
    struct font *pf = font_get(font);
    for (; *text; ++text)
        font_run_next(pf, &run, (unsigned char)*text, NULL);
    font_lock(font, false);
    return run.pen;
}

static int clock_origin(int width, int advance)
{
    return (width * 64 - advance + 1) >> 1;
}

static bool clip_viewport(struct viewport *vp, int x, int y, int w, int h)
{
    int left = MAX(vp->x, x);
    int top = MAX(vp->y, y);
    int right = MIN(vp->x + vp->width, x + w);
    int bottom = MIN(vp->y + vp->height, y + h);

    if (right <= left || bottom <= top)
        return false;

    vp->x = left;
    vp->y = top;
    vp->width = right - left;
    vp->height = bottom - top;
    return true;
}

static void draw_text(struct viewport *damage, const char *text, int font,
                      int x, int y, int width, int height, bool right_aligned)
{
    struct viewport vp = *damage;
    if (!*text || !clip_viewport(&vp, x, y, width, height))
        return;

    if (right_aligned)
    {
        int text_width;
        font_getstringsize((const unsigned char *)text, &text_width, NULL, font);
        /* Scrim places a right-aligned line on the left if it does not fit. */
        x += MAX(0, width - text_width);
    }
    vp.font = font;
    lcd_set_viewport(&vp);
    lcd_putsxy(x - vp.x, y - vp.y, (const unsigned char *)text);
    lcd_set_viewport(damage);
}

static void draw_clock(struct viewport *damage, const char *text, int font,
                       struct ui_rect clip, int origin)
{
    struct viewport vp = *damage;
    if (!clip_viewport(&vp, clip.x, clip.y, clip.width, clip.height))
        return;
#ifdef HAVE_PODBOX_FONT
    font_lock(font, true);
    struct font *pf = font_get(font);
    if (pf->pbf.file_size)
    {
        struct font_run run = { .pen = origin - vp.x * 64 };
        lcd_set_viewport(&vp);
        for (; *text; ++text)
        {
            struct font_glyph glyph;
            font_run_next(pf, &run, (unsigned char)*text, &glyph);
            if (glyph.bits && glyph.width && glyph.height)
                lcd_alpha_bitmap_part(glyph.bits, 0, 0, glyph.width, glyph.x,
                                      clip.y - vp.y + glyph.y, glyph.width, glyph.height);
        }
        lcd_set_viewport(damage);
        font_lock(font, false);
        return;
    }
    font_lock(font, false);
#endif
    int x = (origin + 32) >> 6;
    draw_text(&vp, text, font, x, clip.y, clip.x + clip.width - x, clip.height, false);
    lcd_set_viewport(damage);
}

static unsigned int status_color(const struct ui_theme *theme, enum ui_color_role role,
                                 const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static void draw_volume(const struct native_chrome *chrome, struct viewport *damage,
                        struct ui_rect bounds, const struct ui_album_palette *palette)
{
    const struct ui_theme *theme = chrome->theme;
    const uint16_t *m = theme->metric;
    int icon_size = m[UI_METRIC_STATUS_ICON_SIZE];
    int gap = m[UI_METRIC_STATUS_ICON_GAP];
    int x = bounds.x + m[UI_METRIC_STATUS_PAD_LEFT];
    int right = bounds.x + bounds.width - m[UI_METRIC_STATUS_PAD_RIGHT] - icon_size;
    int icon_y = bounds.y + m[UI_METRIC_STATUS_ICON_Y];
    int bar_x = x + icon_size + gap;
    int bar_width = MAX(0, right - gap - bar_x);
    int bar_height = MAX(1, bounds.height / 6);
    int bar_y = bounds.y + (bounds.height - bar_height) / 2;

    damage->fg_pattern = status_color(theme, UI_COLOR_BACKGROUND, palette);
    lcd_fillrect(0, 0, damage->width, damage->height);
    damage->fg_pattern = status_color(theme, UI_COLOR_FOREGROUND, palette);
    draw_text(damage, chrome->state.volume_percent ? "N" : "M", chrome->icon_font,
              x, icon_y, icon_size, icon_size, false);
    draw_text(damage, "O", chrome->icon_font, right, icon_y, icon_size, icon_size, true);

    damage->fg_pattern = status_color(theme, UI_COLOR_SEPARATOR, palette);
    lcd_fillrect(bar_x - damage->x, bar_y - damage->y, bar_width, bar_height);
    damage->fg_pattern = status_color(theme, UI_COLOR_ACCENT, palette);
    lcd_fillrect(bar_x - damage->x, bar_y - damage->y,
                 bar_width * chrome->state.volume_percent / 100, bar_height);
}

static void draw_indicators(const struct native_chrome *chrome, struct viewport *damage,
                            const struct native_chrome_indicators *icons,
                            struct status_layout layout, int x, int y, int size,
                            unsigned background, unsigned foreground)
{
    for (int i = 0; i < CHROME_GLYPH_COUNT; ++i)
    {
        unsigned ink = icons->ink[i];
        if (!ink)
            continue;
        int slide = (3 * (UINT8_MAX - ink) + 127) / UINT8_MAX;
        damage->fg_pattern = ink == UINT8_MAX ? foreground :
                             color_blend(background, foreground, ink);
        bool battery = i >= CHROME_BATTERY_FIRST;
        int cell = battery ? layout.battery_x : layout.play_x;
        draw_text(damage, indicator_icons[i], chrome->icon_font,
                  x + cell, y + slide, size, size, battery);
    }
    damage->fg_pattern = foreground;
}

void native_chrome_draw(const struct native_chrome *chrome, const char *left,
                        bool playback_modes,
                        const struct ui_view_frame *frame,
                        const struct ui_album_palette *palette,
                        const struct native_chrome_indicators *indicators)
{
    const struct ui_theme *theme = chrome->theme;
    const uint16_t *metric = theme->metric;
    struct ui_rect bounds = frame->bounds;
    struct ui_rect clip = frame->clip;
    struct viewport *previous = lcd_current_viewport;
    struct viewport damage = {
        .x = bounds.x, .y = bounds.y, .width = bounds.width, .height = bounds.height,
        .buffer = previous->buffer,
        .drawmode = DRMODE_FG,
    };
    if (!clip_viewport(&damage, clip.x, clip.y, clip.width, clip.height))
        return;

    lcd_set_viewport(&damage);
    if (!playback_modes && chrome->state.volume_visible)
    {
        draw_volume(chrome, &damage, bounds, palette);
        lcd_set_viewport(previous);
        return;
    }

    unsigned background = status_color(theme, UI_COLOR_STATUS_BG, palette);
    unsigned foreground = status_color(theme, UI_COLOR_STATUS_FG, palette);
    damage.fg_pattern = background;
    lcd_fillrect(0, 0, damage.width, damage.height);
    damage.fg_pattern = foreground;

    const struct native_chrome_state *state = &chrome->state;
    char clock[16];
    if (state->hour < 0)
        strcpy(clock, ":");
    else if (state->clock_24h)
        snprintf(clock, sizeof(clock), "%02d:%02d", state->hour, state->minute);
    else
        snprintf(clock, sizeof(clock), "%d:%02d %s", (state->hour + 11) % 12 + 1,
                 state->minute, state->hour < 12 ? "AM" : "PM");

    char percent[8] = "";
    int percent_width = 0;
    if (state->numeric)
    {
        if (indicators->battery < 0)
            strcpy(percent, "?%");
        else
            snprintf(percent, sizeof(percent), "%d%%", indicators->battery);
        font_getstringsize((const unsigned char *)percent, &percent_width, NULL,
                           chrome->status_font);
    }
    int advance = clock_advance(clock, chrome->status_font);
    int clock_width = 2 * ((advance + 127) / 128);
    struct status_layout layout = status_layout(chrome, bounds.width, clock_width, percent_width);
    int icon_y = bounds.y + metric[UI_METRIC_STATUS_ICON_Y];
    int text_y = bounds.y + metric[UI_METRIC_STATUS_TEXT_Y];
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int left_x = bounds.x + metric[UI_METRIC_STATUS_PAD_LEFT];
    const char *title = title_text(left);
    int title_width = 0;
    if (!playback_modes)
        font_getstringsize((const unsigned char *)title, &title_width, NULL,
                           chrome->title_font);
    const int gap = 8;
    bool right_clock = !playback_modes &&
        left_x + title_width + gap > bounds.x + layout.clock_x;
    int clock_pen = bounds.x * 64 + clock_origin(bounds.width, advance);
    if (right_clock)
    {
        layout.clock_width = clock_width;
        layout.clock_x = bounds.width - metric[UI_METRIC_STATUS_PAD_RIGHT] - clock_width;
        layout.title_width = MAX(0, bounds.x + layout.clock_x - gap - left_x);
        clock_pen = (bounds.x + layout.clock_x) * 64;
        char fitted[260];
        title = text_fit(title, fitted, sizeof(fitted), chrome->title_font,
                         layout.title_width);
        draw_text(&damage, title, chrome->title_font, left_x, text_y,
                  layout.title_width, metric[UI_METRIC_STATUS_TITLE_HEIGHT], false);
    }
    if (playback_modes)
        draw_text(&damage, left, chrome->icon_font, left_x, icon_y,
                  layout.title_width, icon_size, false);
    else if (!right_clock)
        draw_text(&damage, title, chrome->title_font, left_x, text_y,
                  layout.clock_x + bounds.x - left_x - gap,
                  metric[UI_METRIC_STATUS_TITLE_HEIGHT], false);
    if (!right_clock && left_x + title_width <= bounds.x + layout.busy_x &&
        state->busy < ARRAYLEN(busy_icons))
        draw_text(&damage, busy_icons[state->busy], chrome->icon_font,
                  bounds.x + layout.busy_x, icon_y, icon_size, icon_size, false);

    struct ui_rect clock_clip = { bounds.x + layout.clock_x, text_y,
                                  layout.clock_width, icon_size };
    draw_clock(&damage, clock, chrome->status_font, clock_clip, clock_pen);
    if (!right_clock)
    {
        draw_text(&damage, percent, chrome->status_font, bounds.x + layout.percent_x,
                  text_y, layout.percent_width, icon_size, false);
        draw_indicators(chrome, &damage, indicators, layout, bounds.x, icon_y, icon_size,
                        background, foreground);
    }
    lcd_set_viewport(previous);
}
