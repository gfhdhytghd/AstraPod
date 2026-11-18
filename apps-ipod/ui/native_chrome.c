/* Native status bar. Layout adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */
#include "config.h"
#include "native_chrome.h"
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

#ifdef HAVE_USB_PODLINK
#include "podlink.h"
#endif
#ifdef IPOD_ACCESSORY_PROTOCOL
#include "iap.h"
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
#include "usb.h"
#endif

#define BUSY_HIDDEN UINT8_MAX
#define BUSY_TURN ANIM_F(58)
#define VOLUME_HOLD ANIM_F(180)

static const char *const indicator_icons[CHROME_DOCK] = {
    "A", "B", "G", "F",
    "1", "2", "3", "4", "5", "6", "7",
    "·", "¸", "¹", "º",
    "_", "`",
};
static const char *const busy_icons[] = {
    "á", "â", "ã", "ä", "å", "æ", "ç", "è",
};

static unsigned int link_state(void)
{
#ifdef HAVE_USB_PODLINK
    switch (podlink_state())
    {
        case PODLINK_CLOSED: return 0;
        case PODLINK_CONNECTED: return 2;
        default: return 1;
    }
#else
    return 0;
#endif
}

static bool dock_active(void)
{
#ifdef IPOD_ACCESSORY_PROTOCOL
    if (iap_accessory_present())
        return true;
#endif
#if defined(USB_ENABLE_AUDIO) || defined(HAVE_HOST_USB_AUDIO)
    return usb_audio_get_active();
#else
    return false;
#endif
}

void native_chrome_indicators(const struct native_chrome_state *state,
                             struct native_chrome_indicators *out)
{
    *out = (struct native_chrome_indicators){ .battery = -1 };
    switch (state->playmode)
    {
        case STATUS_PLAY: out->ink[CHROME_PLAY] = UINT8_MAX; break;
        case STATUS_PAUSE: out->ink[CHROME_PAUSE] = UINT8_MAX; break;
        case STATUS_FASTFORWARD: out->ink[CHROME_FORWARD] = UINT8_MAX; break;
        case STATUS_FASTBACKWARD: out->ink[CHROME_BACKWARD] = UINT8_MAX; break;
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
    if (state->link)
        out->ink[state->link == 2 ? CHROME_LINK_OPEN : CHROME_LINK_WAIT] = UINT8_MAX;
    if (state->dock)
        out->ink[CHROME_DOCK] = UINT8_MAX;
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
        .link = link_state(),
        .playmode = current_playmode(),
        .dock = dock_active(),
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
    int battery_x, busy_x, clock_x, clock_width, link_x, dock_x, play_x, title_width;
};

static bool has_ink(const struct native_chrome_indicators *icons, int first, int last)
{
    for (int i = first; i <= last; ++i)
        if (icons->ink[i])
            return true;
    return false;
}

static struct status_layout status_layout(const struct native_chrome *chrome, int width,
                                          const struct native_chrome_indicators *icons)
{
    const uint16_t *metric = chrome->theme->metric;
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int battery_x = width - metric[UI_METRIC_STATUS_PAD_RIGHT] - icon_size;
    int busy_x = battery_x - metric[UI_METRIC_STATUS_ICON_GAP] - icon_size;
    int clock_width = metric[chrome->state.numeric ? UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH
                                                  : UI_METRIC_STATUS_CLOCK_WIDTH];
    int clock_x = busy_x - metric[UI_METRIC_STATUS_GROUP_GAP] - clock_width;
    int title_end = clock_x;
    int link_x = title_end - metric[UI_METRIC_STATUS_LINK_GAP] - icon_size;
    if (has_ink(icons, CHROME_LINK_WAIT, CHROME_LINK_OPEN))
        title_end = link_x;
    int dock_x = title_end - metric[UI_METRIC_STATUS_LINK_GAP] - icon_size;
    if (icons->ink[CHROME_DOCK])
        title_end = dock_x;
    int play_x = title_end - metric[UI_METRIC_STATUS_LINK_GAP] - icon_size;
    if (has_ink(icons, CHROME_PLAY, CHROME_BACKWARD))
        title_end = play_x;
    return (struct status_layout) {
        .battery_x = battery_x, .busy_x = busy_x,
        .clock_x = clock_x, .clock_width = clock_width, .link_x = link_x,
        .dock_x = dock_x, .play_x = play_x,
        .title_width = title_end - metric[UI_METRIC_STATUS_PAD_LEFT],
    };
}

static const char *title_text(const char *title)
{
    if (!title)
        return "";
    return strcmp(title, "Rockbox") == 0 ? "PodBox" : title;
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

static void draw_dock(struct viewport *damage, int x, int y, int size)
{
    struct viewport vp = *damage;
    if (!clip_viewport(&vp, x, y, size, size))
        return;
    x += (size - size / 2) / 2 - vp.x;
    y += size / 3 - vp.y;
    int width = MAX(3, size / 2);
    int height = MAX(1, size / 5);
    lcd_set_viewport(&vp);
    lcd_drawrect(x, y, width, height + 1);
    lcd_vline(x + width / 4, y + height, y + height + MAX(1, size / 8));
    lcd_vline(x + width * 3 / 4, y + height, y + height + MAX(1, size / 8));
    lcd_set_viewport(damage);
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
        if (i == CHROME_DOCK)
            draw_dock(damage, x + layout.dock_x, y + slide, size);
        else
        {
            bool battery = i >= CHROME_BATTERY_FIRST && i <= CHROME_CHARGE_LAST;
            int cell = battery ? layout.battery_x :
                       i >= CHROME_LINK_WAIT ? layout.link_x : layout.play_x;
            draw_text(damage, indicator_icons[i], chrome->icon_font,
                      x + cell, y + slide, size, size, battery);
        }
    }
    damage->fg_pattern = foreground;
}

void native_chrome_draw(const struct native_chrome *chrome, const char *title,
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
    if (chrome->state.volume_visible)
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
    struct status_layout layout = status_layout(chrome, bounds.width, indicators);
    int icon_y = bounds.y + metric[UI_METRIC_STATUS_ICON_Y];
    int text_y = bounds.y + metric[UI_METRIC_STATUS_TEXT_Y];
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    draw_text(&damage, title_text(title), chrome->title_font,
              bounds.x + metric[UI_METRIC_STATUS_PAD_LEFT], text_y,
              layout.title_width, metric[UI_METRIC_STATUS_TITLE_HEIGHT], false);

    draw_indicators(chrome, &damage, indicators, layout, bounds.x, icon_y, icon_size,
                    background, foreground);

    if (state->busy < ARRAYLEN(busy_icons))
        draw_text(&damage, busy_icons[state->busy], chrome->icon_font,
                  bounds.x + layout.busy_x, icon_y, icon_size, icon_size, false);

    char clock[24];
    if (state->hour < 0)
        strcpy(clock, ": ");
    else if (state->clock_24h)
        snprintf(clock, sizeof(clock), "%02d:%02d", state->hour, state->minute);
    else
        snprintf(clock, sizeof(clock), "%2d:%02d %s", (state->hour + 11) % 12 + 1,
                 state->minute, state->hour < 12 ? "AM" : "PM");

    if (state->numeric)
    {
        size_t len = strlen(clock);
        if (indicators->battery < 0)
            snprintf(clock + len, sizeof(clock) - len, "  ?%%");
        else
            snprintf(clock + len, sizeof(clock) - len, "  %d%%", indicators->battery);
    }
    draw_text(&damage, clock, chrome->status_font, bounds.x + layout.clock_x,
              text_y, layout.clock_width, icon_size, true);
    lcd_set_viewport(previous);
}
