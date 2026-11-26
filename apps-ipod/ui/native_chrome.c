/* Native status bar. Layout adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */
#include "config.h"
#include "native_chrome.h"
#include "draw/line.h"
#include "native_theme_runtime.h"
#include "retained.h"
#include "theme_art.h"

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
#include "power.h"
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

#ifdef ASTRAPOD_ROCK_COMPAT
    app_tray_snapshot(now, &next.tray);
#endif
    /* The sample has byte alignment and no padding. Compare it to
     * invalidate only the status bar when list geometry is unchanged. */
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
    chrome->title_font = ui_theme_font_load(theme, UI_FONT_STATUS_TITLE, glyphs);
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
                       ui_frame_clock(VOLUME_HOLD) / (1000000 / HZ));
}

struct status_layout {
    int battery_x, percent_x, percent_width, play_x, busy_x;
    int clock_x, clock_width, title_width;
    int tray_x, tray_count;
    bool right_clock;
};

static struct status_layout status_layout(const struct native_chrome *chrome,
                                          int width, int clock_width,
                                          int percent_width, int title_width)
{
    const uint16_t *metric = chrome->theme->metric;
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int gap = metric[UI_METRIC_STATUS_ICON_GAP];
    int group_gap = metric[UI_METRIC_STATUS_GROUP_GAP];
    int left = metric[UI_METRIC_STATUS_PAD_LEFT];
    int battery_x = width - metric[UI_METRIC_STATUS_PAD_RIGHT] -
        (ui_theme_art(chrome->theme) ?
         stock_art[STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_0].width : icon_size);
    int play_x = battery_x - gap - icon_size;
    /* Keep the playback slot even when stopped so the busy icon stays put. */
    int busy_x = play_x - gap - icon_size;
    int percent_limit = busy_x - gap - group_gap - left - clock_width;
    percent_width = MIN(percent_width, MAX(0, percent_limit));
    int percent_x = battery_x - gap - percent_width;
    if (percent_width)
    {
        play_x = percent_x - gap - icon_size;
        busy_x = play_x - gap - icon_size;
    }

    int tray_x = busy_x;
    int tray_count = 0;
#ifdef ASTRAPOD_ROCK_COMPAT
    int tray_pitch = ROCK_TRAY_ICON_SIZE + gap;
    int tray_room = busy_x - group_gap - left - clock_width;
    tray_count = MIN(chrome->state.tray.count, MAX(0, tray_room) / tray_pitch);
    tray_x -= tray_count * tray_pitch;
#endif
    int clock_right = tray_x - group_gap;
    clock_width = MIN(clock_width, MAX(0, clock_right - left));
    int clock_x = (width - clock_width) / 2;
    const int title_gap = 8;
    bool right_clock = left + title_width + title_gap > clock_x ||
                       clock_x + clock_width > clock_right;
    if (right_clock)
        clock_x = clock_right - clock_width;

    return (struct status_layout) {
        .battery_x = battery_x, .percent_x = percent_x, .percent_width = percent_width,
        .play_x = play_x, .busy_x = busy_x,
        .tray_x = tray_x, .tray_count = tray_count,
        .clock_x = clock_x, .clock_width = clock_width,
        .title_width = MAX(0, clock_x - title_gap - left),
        .right_clock = right_clock,
    };
}

const char *native_chrome_title(const char *title)
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

/* A glyph larger than its square slot is centred on the slot, not cut to
 * it; right-aligned, its right edge stays on the slot's. */
static void draw_icon(struct viewport *damage, const char *glyph, int font,
                      int x, int y, int size, bool right_aligned)
{
    int width, height;
    font_getstringsize((const unsigned char *)glyph, &width, &height, font);
    if (width <= size && height <= size)
    {
        draw_text(damage, glyph, font, x, y, size, size, right_aligned);
        return;
    }
    x += right_aligned ? size - width : (size - width) / 2;
    draw_text(damage, glyph, font, x, y + (size - height) / 2, width, height,
              false);
}

/* Fill the damaged part of a status bar `height` rows tall from its top. */
static void fill_gradient(struct viewport *damage, int top, int height,
                          const struct ui_theme_gradient *gradient)
{
    for (int y = 0; y < damage->height; ++y)
    {
        uint32_t rgb = ui_theme_gradient_at(gradient, damage->y + y - top,
                                            height, 0);
        damage->fg_pattern = LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255,
                                         rgb & 255);
        lcd_fillrect(0, y, damage->width, 1);
    }
}

/* Stock's status bar is one bitmap, the same in every column. */
static void fill_art(const struct viewport *damage, int top)
{
    stock_art_draw(STOCK_ART_STATUSBARWHITE_BACKGROUND, damage->x, top,
                   (struct ui_rect){ damage->x, damage->y, damage->width,
                                     damage->height });
}

/* The bar's colour at row `y` of its `height`, as 0xRRGGBB. */
static uint32_t bar_rgb(const struct ui_theme *theme, int y, int height)
{
    if (ui_theme_art(theme) && stock_art_ready())
    {
        const struct stock_art *art =
            &stock_art[STOCK_ART_STATUSBARWHITE_BACKGROUND];
        int i = MIN(MAX(y, 0), art->height - 1) * art->width;
        unsigned n = art->colors <= 256 ? ((const uint8_t *)art->pixels)[i] :
                                          ((const uint16_t *)art->pixels)[i];
        return art->palette[n] & 0xffffff;
    }
    return ui_theme_gradient_at(&theme->gradient[UI_GRADIENT_STATUS], y,
                                height, 0);
}

/* A bar with gradient.status embosses its title as stock does: the title in
 * white at 144/255 one row lower, under the title. Little of it shows but
 * the row below the baseline, so it is drawn in the white blended over the
 * bar on that row. */
static void draw_emboss(struct viewport *damage, const struct ui_theme *theme,
                        const char *title, int font, int x, int y, int width,
                        int height, struct ui_rect bounds)
{
    int row = y + font_get(font)->ascent - bounds.y;
    uint32_t bar = bar_rgb(theme, row, bounds.height);
    const unsigned alpha = 144 + (144 >> 7);
    unsigned c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = (((bar >> (16 - 8 * i)) & 255) * (256 - alpha) +
                255 * alpha) >> 8;
    unsigned foreground = damage->fg_pattern;
    damage->fg_pattern = LCD_RGBPACK(c[0], c[1], c[2]);
    draw_text(damage, title, font, x, y + 1, width, height, false);
    damage->fg_pattern = foreground;
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
                lcd_font_glyph(&glyph, 0, glyph.x, clip.y - vp.y + glyph.y);
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

    const struct ui_theme_gradient *gradient =
        &theme->gradient[UI_GRADIENT_STATUS];
    if (ui_theme_art(theme))
    {
        fill_art(damage, bounds.y);
        damage->fg_pattern = status_color(theme, UI_COLOR_STATUS_FG, palette);
    }
    else if (gradient->stops)
    {
        fill_gradient(damage, bounds.y, bounds.height, gradient);
        damage->fg_pattern = status_color(theme, UI_COLOR_STATUS_FG, palette);
    }
    else
    {
        damage->fg_pattern = status_color(theme, UI_COLOR_BACKGROUND, palette);
        lcd_fillrect(0, 0, damage->width, damage->height);
        damage->fg_pattern = status_color(theme, UI_COLOR_FOREGROUND, palette);
    }
    draw_icon(damage, chrome->state.volume_percent ? "N" : "M",
              chrome->icon_font, x, icon_y, icon_size, false);
    draw_icon(damage, "O", chrome->icon_font, right, icon_y, icon_size, true);

    damage->fg_pattern = status_color(theme, UI_COLOR_SEPARATOR, palette);
    lcd_fillrect(bar_x - damage->x, bar_y - damage->y, bar_width, bar_height);
    damage->fg_pattern = status_color(theme, UI_COLOR_ACCENT, palette);
    lcd_fillrect(bar_x - damage->x, bar_y - damage->y,
                 bar_width * chrome->state.volume_percent / 100, bar_height);
}

/* Stock draws the battery 3 rows down and the playback icon 2 rows down,
 * right-aligned in its slot. A charging battery wears a bolt, and a charged
 * one still on power a plug. */
static void draw_art_indicators(const struct native_chrome *chrome,
                                const struct viewport *damage,
                                const struct native_chrome_indicators *icons,
                                struct status_layout layout, int x, int y,
                                int size)
{
    const struct native_chrome_state *state = &chrome->state;
    struct ui_rect clip = { damage->x, damage->y, damage->width,
                            damage->height };
    enum stock_art_id level = ui_art_battery(MAX(state->battery, 0));
    uint32_t pixels[26 * 13] = { 0 };
    struct stock_canvas canvas = {
        pixels, x + layout.battery_x, y + 3, stock_art[level].width,
        stock_art[level].height,
    };
    /* The bar, the battery and its bolt are composed at 8 bits a channel
     * and truncated once, as stock composes them. */
    if (state->battery >= 0 &&
        canvas.width * canvas.height <= (int)ARRAYLEN(pixels))
    {
        int bar_top = y - chrome->theme->metric[UI_METRIC_STATUS_ICON_Y];
        stock_art_put_part(&canvas, STOCK_ART_STATUSBARWHITE_BACKGROUND, 0,
                           canvas.y - bar_top, canvas.width, canvas.height,
                           canvas.x, canvas.y);
        stock_art_put(&canvas, level, canvas.x, canvas.y);
        if (state->charging)
            stock_art_put(&canvas,
                          STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_CHARGE,
                          canvas.x, canvas.y);
#if CONFIG_CHARGING
        else if (charger_inserted())
            stock_art_put(&canvas, STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_PLUG,
                          canvas.x, canvas.y);
#endif
        stock_canvas_show(&canvas, clip);
    }
    int play = icons->ink[CHROME_PLAY] ? STOCK_ART_NOWPLAYING_WHITE_PLAY :
               icons->ink[CHROME_PAUSE] ? STOCK_ART_NOWPLAYING_WHITE_PAUSE : -1;
    if (play >= 0)
        stock_art_draw(play, x + layout.play_x + size - stock_art[play].width,
                       y + 2, clip);
}

static void draw_indicators(const struct native_chrome *chrome, struct viewport *damage,
                            const struct native_chrome_indicators *icons,
                            struct status_layout layout, int x, int y, int size,
                            unsigned background, unsigned foreground)
{
    bool art = ui_theme_art(chrome->theme);
    if (art)
        draw_art_indicators(chrome, damage, icons, layout, x, y, size);
    for (int i = 0; i < CHROME_GLYPH_COUNT && !art; ++i)
    {
        unsigned ink = icons->ink[i];
        if (!ink)
            continue;
        int slide = (3 * (UINT8_MAX - ink) + 127) / UINT8_MAX;
        damage->fg_pattern = ink == UINT8_MAX ? foreground :
                             color_blend(background, foreground, ink);
        bool battery = i >= CHROME_BATTERY_FIRST;
        int cell = battery ? layout.battery_x : layout.play_x;
        draw_icon(damage, indicator_icons[i], chrome->icon_font,
                  x + cell, y + slide, size, battery);
    }
    damage->fg_pattern = foreground;
    if (chrome->state.busy < ARRAYLEN(busy_icons))
        draw_icon(damage, busy_icons[chrome->state.busy], chrome->icon_font,
                  x + layout.busy_x, y, size, false);
}

void native_chrome_draw(const struct native_chrome *chrome, const char *left,
                        bool playback_modes,
                        const struct ui_view_frame *frame,
                        const struct ui_album_palette *palette,
                        const struct native_chrome_indicators *indicators,
                        const struct native_chrome_colors *colors)
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

    unsigned background = colors ? colors->background :
                          status_color(theme, UI_COLOR_STATUS_BG, palette);
    unsigned foreground = colors ? colors->foreground :
                          status_color(theme, UI_COLOR_STATUS_FG, palette);
    const struct ui_theme_gradient *gradient =
        &theme->gradient[UI_GRADIENT_STATUS];
    if (ui_theme_art(theme))
        fill_art(&damage, bounds.y);
    else if (gradient->stops)
        fill_gradient(&damage, bounds.y, bounds.height, gradient);
    else
    {
        damage.fg_pattern = background;
        lcd_fillrect(0, 0, damage.width, damage.height);
    }
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
    bool show_clock = theme->style[UI_STYLE_STATUS_CLOCK] == UI_STATUS_CLOCK_ON;
    bool center_title =
        theme->style[UI_STYLE_STATUS_TITLE] == UI_STATUS_TITLE_CENTER;
    int advance = show_clock ? clock_advance(clock, chrome->status_font) : 0;
    int clock_width = 2 * ((advance + 127) / 128);
    int icon_y = bounds.y + metric[UI_METRIC_STATUS_ICON_Y];
    int text_y = bounds.y + metric[UI_METRIC_STATUS_TEXT_Y];
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int left_x = bounds.x + metric[UI_METRIC_STATUS_PAD_LEFT];
    const char *title = native_chrome_title(left);
    int title_width = 0;
    if (!playback_modes)
        font_getstringsize((const unsigned char *)title, &title_width, NULL,
                           chrome->title_font);
    /* A centred title keeps the clock at the right of the bar. */
    struct status_layout layout = status_layout(chrome, bounds.width,
                                                clock_width, percent_width,
                                                center_title ? bounds.width :
                                                title_width);
    if (!show_clock)
        layout.title_width = MAX(0, layout.tray_x -
                                    metric[UI_METRIC_STATUS_GROUP_GAP] -
                                    metric[UI_METRIC_STATUS_PAD_LEFT]);
    if (center_title && title_width < layout.title_width)
    {
        int title_x = MIN((bounds.width - title_width) / 2,
                          metric[UI_METRIC_STATUS_PAD_LEFT] +
                          layout.title_width - title_width);
        left_x = bounds.x + MAX(title_x, metric[UI_METRIC_STATUS_PAD_LEFT]);
        layout.title_width = title_width;
    }
    if (playback_modes)
        draw_text(&damage, left, chrome->icon_font, left_x, icon_y,
                  layout.title_width, icon_size, false);
    else
    {
        char fitted[260];
        if (title_width > layout.title_width)
            title = text_fit(title, fitted, sizeof(fitted), chrome->title_font,
                             layout.title_width);
        if (gradient->stops)
            draw_emboss(&damage, theme, title, chrome->title_font, left_x,
                        text_y, layout.title_width,
                        metric[UI_METRIC_STATUS_TITLE_HEIGHT], bounds);
        draw_text(&damage, title, chrome->title_font, left_x, text_y,
                  layout.title_width, metric[UI_METRIC_STATUS_TITLE_HEIGHT], false);
    }

    int clock_pen = bounds.x * 64 + (layout.right_clock ? layout.clock_x * 64 :
                                    clock_origin(bounds.width, advance));
    struct ui_rect clock_clip = { bounds.x + layout.clock_x, text_y,
                                  layout.clock_width, icon_size };
    if (show_clock)
        draw_clock(&damage, clock, chrome->status_font, clock_clip, clock_pen);
    draw_text(&damage, percent, chrome->status_font,
              bounds.x + layout.percent_x, text_y, layout.percent_width,
              icon_size, false);
    draw_indicators(chrome, &damage, indicators, layout, bounds.x, icon_y,
                    icon_size, background, foreground);
#ifdef ASTRAPOD_ROCK_COMPAT
    int tray_x = bounds.x + layout.tray_x - damage.x;
    int tray_y = bounds.y + (bounds.height - ROCK_TRAY_ICON_SIZE) / 2 - damage.y;
    for (int i = 0; i < layout.tray_count; ++i)
    {
        lcd_mono_bitmap(state->tray.icons[i].columns, tray_x, tray_y,
                        ROCK_TRAY_ICON_SIZE, ROCK_TRAY_ICON_SIZE);
        tray_x += ROCK_TRAY_ICON_SIZE + metric[UI_METRIC_STATUS_ICON_GAP];
    }
#endif
    lcd_set_viewport(previous);
}
