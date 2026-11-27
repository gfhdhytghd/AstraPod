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
#include "system/battery_calibration.h"
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
#if defined(IPOD_6G) && !defined(SIMULATOR)
    battery = battery_calibration_display(battery);
#endif
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
    if (battery >= 0 && ui_theme_art(chrome->theme))
        next.stock_battery_frame = ui_art_battery_index(battery);
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
    /* The sample has no padding. Compare it to
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
#ifdef ASTRAPOD_ROCK_COMPAT
    int tray_icon_x[APP_TRAY_VISIBLE];
#endif
    bool right_clock;
};

/* Stock leaves 7 clear columns between its playback icon and the battery.
 * The stock look keeps that between the ink of every indicator, packed
 * right to left from the battery: the playback icon when shown, the busy
 * spinner when shown, then the app tray. A clock sits left of them, and
 * the title takes what remains. */
#define ART_INK_GAP 7
/* Stock's play art inks columns 1 to 11 of its 14; pause sits within. */
#define ART_PLAY_INK_LEFT 1
#define ART_PLAY_INK_RIGHT 11
/* Every busy frame of the stock icon font inks the middle 12 columns. */
#define ART_BUSY_INK 12

#ifdef ASTRAPOD_ROCK_COMPAT
static int tray_ink(const struct rock_tray_icon12 *icon, int *first)
{
    int last = -1;
    *first = 0;
    for (int c = 0; c < ROCK_TRAY_ICON12_SIZE; ++c)
        if (icon->columns[c])
        {
            if (last < 0)
                *first = c;
            last = c;
        }
    return last;
}
#endif

static struct status_layout art_layout(const struct native_chrome *chrome,
                                       int width, int clock_width,
                                       int percent_width, bool play_shown)
{
    const uint16_t *metric = chrome->theme->metric;
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int gap = metric[UI_METRIC_STATUS_ICON_GAP];
    int group_gap = metric[UI_METRIC_STATUS_GROUP_GAP];
    int left = metric[UI_METRIC_STATUS_PAD_LEFT];
    struct status_layout out = {
        .battery_x = width - metric[UI_METRIC_STATUS_PAD_RIGHT] -
            stock_art[STOCK_ART_STATUSBARWHITE_BATTERY_IMAGE_0].width,
        .right_clock = true,
    };
    out.percent_width = MIN(percent_width, MAX(0, out.battery_x - gap - left));
    out.percent_x = out.battery_x - gap - out.percent_width;
    int edge = out.percent_width ? out.percent_x : out.battery_x;

    /* The playback art is drawn right-aligned in its cell. */
    int play_left = edge - ART_INK_GAP - (ART_PLAY_INK_RIGHT + 1);
    out.play_x = play_left + stock_art[STOCK_ART_NOWPLAYING_WHITE_PLAY].width -
                 icon_size;
    if (play_shown)
        edge = play_left + ART_PLAY_INK_LEFT;
    /* Where draw_icon puts the glyph in its slot, then the ink in that. */
    int busy_width, busy_height;
    font_getstringsize((const unsigned char *)busy_icons[0], &busy_width,
                       &busy_height, chrome->icon_font);
    int busy_ink = (busy_width - ART_BUSY_INK) / 2;
    if (busy_width > icon_size || busy_height > icon_size)
        busy_ink += (icon_size - busy_width) / 2;
    out.busy_x = edge - ART_INK_GAP - ART_BUSY_INK - busy_ink;
    if (chrome->state.busy < ARRAYLEN(busy_icons))
        edge = out.busy_x + busy_ink;

#ifdef ASTRAPOD_ROCK_COMPAT
    const struct app_tray_view *tray = &chrome->state.tray;
    int room = edge - group_gap - left - clock_width;
    for (int first; out.tray_count < tray->count; ++out.tray_count)
    {
        int need = ART_INK_GAP + 1 +
                   tray_ink(&tray->icons[out.tray_count], &first) - first;
        if (need > room)
            break;
        room -= need;
    }
    for (int i = out.tray_count, first; i-- > 0; )
    {
        int last = tray_ink(&tray->icons[i], &first);
        edge -= ART_INK_GAP;
        out.tray_icon_x[i] = edge - 1 - last;
        edge -= last + 1 - first;
    }
#endif
    out.tray_x = edge;
    int clock_right = edge - group_gap;
    out.clock_width = MIN(clock_width, MAX(0, clock_right - left));
    out.clock_x = clock_right - out.clock_width;
    const int title_gap = 8;
    out.title_width = MAX(0, out.clock_x - title_gap - left);
    return out;
}

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
    int tray_pitch = ROCK_TRAY_ICON12_SIZE + gap;
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

    struct status_layout out = {
        .battery_x = battery_x, .percent_x = percent_x, .percent_width = percent_width,
        .play_x = play_x, .busy_x = busy_x,
        .tray_x = tray_x, .tray_count = tray_count,
        .clock_x = clock_x, .clock_width = clock_width,
        .title_width = MAX(0, clock_x - title_gap - left),
        .right_clock = right_clock,
    };
#ifdef ASTRAPOD_ROCK_COMPAT
    for (int i = 0; i < tray_count; ++i)
        out.tray_icon_x[i] = tray_x + i * tray_pitch;
#endif
    return out;
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

/* The ARGB of image `id` at (x, y); the pack must be loaded. */
static uint32_t art_pixel(enum stock_art_id id, int x, int y)
{
    const struct stock_art *art = &stock_art[id];
    int i = y * art->width + x;
    unsigned n = art->colors <= 256 ? ((const uint8_t *)art->pixels)[i] :
                                      ((const uint16_t *)art->pixels)[i];
    return art->palette[n];
}

/* The bar's colour at row `y` of its `height`, as 0xRRGGBB. */
static uint32_t bar_rgb(const struct ui_theme *theme, int y, int height)
{
    if (ui_theme_art(theme) && stock_art_ready())
    {
        int last = stock_art[STOCK_ART_STATUSBARWHITE_BACKGROUND].height - 1;
        return art_pixel(STOCK_ART_STATUSBARWHITE_BACKGROUND, 0,
                         MIN(MAX(y, 0), last)) & 0xffffff;
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
    enum stock_art_id level = ui_art_battery(state->stock_battery_frame);
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

#ifdef ASTRAPOD_ROCK_COMPAT
/* Stock's pause art is two bars of 12 rows of ink, from its row 1: a blue
 * lit brightest at the middle row, darker on the outline, over a row of
 * white highlight. A tray icon on the art bar takes each pixel from the
 * pause pixel on the same row, so it shares the line and the shading. */
#define ART_TRAY_ROW 1
#define ART_TRAY_OUTLINE 1
#define ART_TRAY_INSIDE 2
#define ART_TRAY_HIGHLIGHT 13

static void draw_art_tray(const struct rock_tray_icon12 *icon, int x, int y,
                          int bar_top, struct ui_rect clip)
{
    enum { SIZE = ROCK_TRAY_ICON12_SIZE };
    const enum stock_art_id pause = STOCK_ART_NOWPLAYING_WHITE_PAUSE;
    uint32_t pixels[SIZE * (SIZE + 1)];
    struct stock_canvas canvas = { pixels, x, y, SIZE, SIZE + 1 };
    stock_art_put_part(&canvas, STOCK_ART_STATUSBARWHITE_BACKGROUND, 0,
                       y - bar_top, SIZE, SIZE + 1, x, y);
    for (int c = 0; c < SIZE; ++c)
    {
        unsigned ink = icon->columns[c];
        unsigned left = c > 0 ? icon->columns[c - 1] : 0;
        unsigned right = c + 1 < SIZE ? icon->columns[c + 1] : 0;
        unsigned inside = ink & left & right & ink << 1 & ink >> 1;
        for (int r = 0; r < SIZE; ++r)
        {
            if (!(ink >> r & 1))
                continue;
            int sx = inside >> r & 1 ? ART_TRAY_INSIDE : ART_TRAY_OUTLINE;
            uint32_t argb = art_pixel(pause, sx, ART_TRAY_ROW + r);
            stock_canvas_blend(&canvas, x + c, y + r, argb, argb >> 24);
        }
        if (ink)
        {
            uint32_t argb = art_pixel(pause, ART_TRAY_INSIDE,
                                      ART_TRAY_HIGHLIGHT);
            int bottom = 31 - __builtin_clz(ink);
            stock_canvas_blend(&canvas, x + c, y + bottom + 1, argb,
                               argb >> 24);
        }
    }
    stock_canvas_show(&canvas, clip);
}

/* Shaded on the art bar; elsewhere a mask in the status foreground, centred
 * on the bar. */
static void draw_tray(const struct native_chrome *chrome,
                      struct viewport *damage, struct status_layout layout,
                      struct ui_rect bounds, int icon_y)
{
    const struct app_tray_view *tray = &chrome->state.tray;
    bool art = ui_theme_art(chrome->theme) && stock_art_ready();
    /* The pause art's ink rows start 2 + ART_TRAY_ROW below the icon row. */
    int y = art ? icon_y + 2 + ART_TRAY_ROW :
            bounds.y + (bounds.height - ROCK_TRAY_ICON12_SIZE) / 2;
    struct ui_rect clip = { damage->x, damage->y, damage->width,
                            damage->height };
    for (int i = 0; i < layout.tray_count; ++i)
    {
        const struct rock_tray_icon12 *icon = &tray->icons[i];
        int x = bounds.x + layout.tray_icon_x[i];
        if (art)
        {
            draw_art_tray(icon, x, y, bounds.y, clip);
            continue;
        }
        for (int c = 0; c < ROCK_TRAY_ICON12_SIZE; ++c)
            for (int r = 0; r < ROCK_TRAY_ICON12_SIZE; ++r)
                if (icon->columns[c] >> r & 1)
                    lcd_drawpixel(x + c - damage->x, y + r - damage->y);
    }
}
#endif

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
    bool play_shown = indicators->ink[CHROME_PLAY] ||
                      indicators->ink[CHROME_PAUSE];
    /* A centred title keeps the clock at the right of the bar. */
    struct status_layout layout = ui_theme_art(theme) ?
        art_layout(chrome, bounds.width, clock_width, percent_width,
                   play_shown) :
        status_layout(chrome, bounds.width, clock_width, percent_width,
                      center_title ? bounds.width : title_width);
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
    draw_tray(chrome, &damage, layout, bounds, icon_y);
#endif
    lcd_set_viewport(previous);
}
