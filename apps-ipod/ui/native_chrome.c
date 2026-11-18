/* Native status bar. Layout adapted from Scrim by Anthony Fletcher
 * (CC BY-SA 3.0). GPL version 2+. */
#include "config.h"
#include "native_chrome.h"
#include "native_theme_runtime.h"
#include "retained.h"

#include <stdio.h>
#include <string.h>

#include "animation.h"
#include "database/tagcache.h"
#include "files/file_index.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "led.h"
#include "metadata/art_cache.h"
#include "powermgmt.h"
#include "settings/settings.h"
#include "system.h"
#include "system/activity.h"
#include "timefuncs.h"

#ifdef HAVE_USB_PODLINK
#include "podlink.h"
#endif

#define BUSY_HIDDEN UINT8_MAX
#define BUSY_TURN ANIM_F(58)
#define LEGACY_VOLUME_HOLD ANIM_F(180)

static const char *const battery_icons[] = {
    "1", "1", "2", "3", "4", "5", "6", "7",
};
static const char *const charging_icons[] = {
    "·", "·", "¸", "¸", "¸", "¸", "¹", "º",
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
        .busy = busy_state(),
    };

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
                       LEGACY_VOLUME_HOLD * HZ / UI_ANIM_FPS);
}

struct status_layout {
    int battery_x, busy_x, clock_x, clock_width, link_x, title_width;
};

static struct status_layout status_layout(const struct native_chrome *chrome, int width)
{
    const uint16_t *metric = chrome->theme->metric;
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    int battery_x = width - metric[UI_METRIC_STATUS_PAD_RIGHT] - icon_size;
    int busy_x = battery_x - metric[UI_METRIC_STATUS_ICON_GAP] - icon_size;
    int clock_width = metric[chrome->state.numeric ? UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH
                                                  : UI_METRIC_STATUS_CLOCK_WIDTH];
    int clock_x = busy_x - metric[UI_METRIC_STATUS_GROUP_GAP] - clock_width;
    int link_x = clock_x - metric[UI_METRIC_STATUS_LINK_GAP] - icon_size;
    int title_end = chrome->state.link ? link_x : clock_x;
    return (struct status_layout) {
        .battery_x = battery_x, .busy_x = busy_x,
        .clock_x = clock_x, .clock_width = clock_width, .link_x = link_x,
        .title_width = title_end - metric[UI_METRIC_STATUS_PAD_LEFT],
    };
}

static const char *title_text(const char *title)
{
    if (!title)
        return "";
    return strcmp(title, "Rockbox") == 0 ? "PodBox" : title;
}

bool native_chrome_title_fits(const struct native_chrome *chrome, const char *title)
{
    int width;
    font_getstringsize((const unsigned char *)title_text(title), &width, NULL,
                       chrome->title_font);
    return width <= status_layout(chrome, LCD_WIDTH).title_width;
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

void native_chrome_draw(const struct native_chrome *chrome, const char *title,
                        const struct ui_view_frame *frame,
                        const struct ui_album_palette *palette)
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

    damage.fg_pattern = status_color(theme, UI_COLOR_STATUS_BG, palette);
    lcd_set_viewport(&damage);
    lcd_fillrect(0, 0, damage.width, damage.height);
    damage.fg_pattern = status_color(theme, UI_COLOR_STATUS_FG, palette);

    const struct native_chrome_state *state = &chrome->state;
    struct status_layout layout = status_layout(chrome, bounds.width);
    int icon_y = bounds.y + metric[UI_METRIC_STATUS_ICON_Y];
    int text_y = bounds.y + metric[UI_METRIC_STATUS_TEXT_Y];
    int icon_size = metric[UI_METRIC_STATUS_ICON_SIZE];
    draw_text(&damage, title_text(title), chrome->title_font,
              bounds.x + metric[UI_METRIC_STATUS_PAD_LEFT], text_y,
              layout.title_width, metric[UI_METRIC_STATUS_TITLE_HEIGHT], false);

    int band = state->battery < 0 ? 0 : state->battery * 6 / 100 + 1;
    const char *const *icons = state->charging ? charging_icons : battery_icons;
    draw_text(&damage, icons[band], chrome->icon_font, bounds.x + layout.battery_x,
              icon_y, icon_size, icon_size, true);

    if (state->busy < ARRAYLEN(busy_icons))
        draw_text(&damage, busy_icons[state->busy], chrome->icon_font,
                  bounds.x + layout.busy_x, icon_y, icon_size, icon_size, false);
    if (state->link)
        draw_text(&damage, state->link == 2 ? "`" : "_", chrome->icon_font,
                  bounds.x + layout.link_x, icon_y, icon_size, icon_size, false);

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
        if (state->battery < 0)
            snprintf(clock + len, sizeof(clock) - len, "  ?%%");
        else
            snprintf(clock + len, sizeof(clock) - len, "  %d%%", state->battery);
    }
    draw_text(&damage, clock, chrome->status_font, bounds.x + layout.clock_x,
              text_y, layout.clock_width, icon_size, true);
    lcd_set_viewport(previous);
}
