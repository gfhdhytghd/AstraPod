/* Native status bar service. GPL version 2+. */
#include "config.h"
#include "plugins/rock_compat.h"
#include "statusbar.h"

#include <string.h>

#include "animation.h"
#include "audio/play_status.h"
#include "button.h"
#include "core_alloc.h"
#include "database/tagcache.h"
#include "draw/color.h"
#include "draw/viewport.h"
#include "files/file_index.h"
#include "frame.h"
#include "kernel.h"
#include "lcd.h"
#include "led.h"
#include "metadata/album_blur.h"
#include "metadata/album_palette.h"
#include "metadata/art_cache.h"
#include "native_chrome.h"
#include "native_theme_runtime.h"
#include "native_wps.h"
#include "perf.h"
#include "retained.h"
#include "settings/settings.h"
#include "string-extra.h"
#include "system.h"
#include "system/activity.h"

#define BUSY_REDRAW_FRAMES ANIM_F(8)
#define IDLE_REDRAW_FRAMES ANIM_F(60)
#define INDICATOR_FRAMES ANIM_F(8)

struct status_title {
    char text[260];
    enum themable_icons icon;
    bool persistent;
    bool dirty;
};

struct indicator_motion {
    struct native_chrome_indicators from, target, visible;
    struct ui_tween fade;
    unsigned sampled_frame;
    bool ready;
};

struct statusbar {
    struct ui_theme theme;
    struct native_chrome chrome;
    struct indicator_motion motion;
    struct status_title title[NB_SCREENS];
    struct native_chrome_colors colors;
    unsigned tint, preferred;
    bool adaptive, has_tint;
    unsigned generation, palette_generation;
    long next_update;
    bool fonts_ready, fonts_tried;
};

static int status_handle;
static int update_delay = BUSY_REDRAW_FRAMES * HZ / UI_ANIM_FPS;
static bool invalidated;
static volatile bool indicators_moving;
static uint32_t motion_start;
static unsigned motion_duration;
static unsigned motion_trace, final_flush_count;
static bool trace_finished, trace_needs_flush;

static int mix_indicator(int from, int to, int amount)
{
    return (from * (UINT8_MAX - amount) + to * amount + UINT8_MAX / 2) / UINT8_MAX;
}

static bool advance_indicators(struct indicator_motion *motion,
                               const struct native_chrome_indicators *target,
                               uint32_t now, unsigned frames)
{
    struct native_chrome_indicators previous = motion->visible;
    if (!motion->ready || !frames)
    {
        motion->ready = true;
        motion->visible = motion->target = *target;
        motion->fade.frames = 0;
    }
    else
    {
        if (motion->fade.frames)
        {
            unsigned frame = ui_clock_frame(now - motion->fade.start_clock);
            if (frame >= motion->fade.frames)
            {
                motion->visible = motion->target;
                motion->fade.frames = 0;
            }
            else if (frame != motion->sampled_frame)
            {
                int amount = ui_tween_sample(&motion->fade, frame);
                for (int i = 0; i < CHROME_GLYPH_COUNT; ++i)
                    motion->visible.ink[i] = mix_indicator(motion->from.ink[i],
                                                           motion->target.ink[i], amount);
                int from = motion->from.battery;
                int to = motion->target.battery;
                motion->visible.battery = from >= 0 && to >= 0 ?
                    mix_indicator(from, to, amount) : amount >= 128 ? to : from;
                motion->sampled_frame = frame;
            }
        }
        if (memcmp(&motion->target, target, sizeof(*target)))
        {
            motion->from = motion->visible;
            motion->target = *target;
            motion->sampled_frame = 0;
            motion->fade = (struct ui_tween) {
                .start_clock = now, .from = 0, .to = UINT8_MAX, .frames = frames,
            };
        }
    }
    return memcmp(&previous, &motion->visible, sizeof(previous)) != 0;
}

static void retire_trace(bool needs_flush)
{
    if (motion_trace)
    {
        trace_finished = true;
        trace_needs_flush = needs_flush;
        final_flush_count = ui_flush_count();
    }
}

void sb_motion_presented(void)
{
    if (trace_finished && (!trace_needs_flush || ui_flush_count() != final_flush_count))
    {
        unsigned token = motion_trace;
        trace_finished = false;
        motion_trace = 0;
        ui_perf_end(token);
    }
}

int sb_motion_timeout(int timeout)
{
    if (rock_ui_raw() || !indicators_moving || !lcd_active() || ui_flush_inhibited() ||
        lcd_updates_held() || !viewportmanager_theme_enabled(SCREEN_MAIN))
        return timeout;
    int next = ui_clock_poll_timeout(USEC_TIMER - motion_start, motion_duration, HZ);
    return timeout < 0 || next < timeout ? next : timeout;
}

static void refresh_theme(struct statusbar *bar)
{
    unsigned generation = ui_theme_generation();
    if (bar->generation == generation)
        return;
    if (bar->fonts_ready)
        native_chrome_close(&bar->chrome);
    ui_theme_snapshot(&bar->theme);
    bar->generation = generation;
    bar->fonts_ready = bar->fonts_tried = false;
    bar->motion.ready = false;
    invalidated = true;
}

static bool set_title(const char *title, enum themable_icons icon,
                      enum screen_type screen, bool persistent)
{
    if (status_handle > 0)
    {
        struct statusbar *bar = core_get_data_pinned(status_handle);
        struct status_title *state = &bar->title[screen];
        if (!title)
            title = "";
        if (strncmp(state->text, title, sizeof(state->text) - 1))
        {
            strlcpy(state->text, title, sizeof(state->text));
            state->dirty = true;
        }
        state->icon = icon;
        state->persistent = persistent;
        core_put_data_pinned(bar);
    }
    return status_handle > 0 && viewportmanager_theme_enabled(screen);
}

bool sb_set_title_text(const char *title, enum themable_icons icon,
                       enum screen_type screen)
{
    return set_title(title, icon, screen, false);
}

bool sb_set_persistent_title(const char *title, enum themable_icons icon,
                             enum screen_type screen)
{
    return set_title(title, icon, screen, title != NULL);
}

bool sb_get_persistent_title(enum screen_type screen, char *text, size_t size,
                             enum themable_icons *icon)
{
    if (status_handle <= 0)
        return false;
    struct statusbar *bar = core_get_data_pinned(status_handle);
    const struct status_title *state = &bar->title[screen];
    bool persistent = state->persistent;
    if (persistent)
    {
        strlcpy(text, state->text, size);
        if (icon)
            *icon = state->icon;
    }
    core_put_data_pinned(bar);
    return persistent;
}

void sb_clear_transient_title(enum screen_type screen)
{
    if (status_handle <= 0)
        return;
    struct statusbar *bar = core_get_data_pinned(status_handle);
    struct status_title *state = &bar->title[screen];
    if (!state->persistent && state->text[0])
    {
        state->text[0] = '\0';
        state->dirty = true;
    }
    core_put_data_pinned(bar);
}

static unsigned int viewport_color(const struct ui_theme *theme, enum ui_color_role role,
                                    const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static bool refresh_status_colors(struct statusbar *bar)
{
    bool adaptive = global_settings.status_cover_tint &&
                    bar->theme.background[UI_BG_STATUS] == UI_BG_COVER_TINT;
    bool changed = bar->adaptive != adaptive;
    bar->adaptive = adaptive;
    if (!adaptive)
    {
        bar->has_tint = false;
        return changed;
    }

    if (current_playmode() == STATUS_STOP)
        bar->has_tint = false;
    else
    {
        unsigned height = bar->theme.metric[UI_METRIC_STATUS_HEIGHT];
        switch (album_blur_status_tint(height, &bar->tint))
        {
            case ALBUM_BLUR_TINT: bar->has_tint = true; break;
            case ALBUM_BLUR_NO_ART: bar->has_tint = false; break;
            case ALBUM_BLUR_WAITING: break;
        }
    }
    unsigned background = bar->has_tint ? bar->tint :
                          viewport_color(&bar->theme, UI_COLOR_STATUS_BG, NULL);
    unsigned preferred = viewport_color(&bar->theme, UI_COLOR_STATUS_FG, NULL);
    if (changed || bar->colors.background != background || bar->preferred != preferred)
    {
        unsigned foreground = color_srgb_readable_text(preferred, background);
        changed |= bar->colors.background != background || bar->colors.foreground != foreground;
        bar->colors = (struct native_chrome_colors){background, foreground};
        bar->preferred = preferred;
    }
    return changed;
}

void sb_apply_viewport(struct viewport *vp, enum screen_type screen, bool chrome)
{
    if (status_handle <= 0)
        return;
    struct statusbar *bar = core_get_data_pinned(status_handle);
    refresh_theme(bar);
    const struct ui_theme *theme = &bar->theme;
    if (chrome && screen == SCREEN_MAIN)
    {
        const uint16_t *m = theme->metric;
        vp->x = m[UI_METRIC_LIST_X];
        vp->y = m[UI_METRIC_LIST_Y];
        vp->width -= vp->x * 2;
        vp->height -= vp->y + m[UI_METRIC_LIST_BOTTOM];
    }
    struct ui_album_palette palette;
    album_palette_snapshot(&palette);
    vp->fg_pattern = viewport_color(theme, UI_COLOR_FOREGROUND, &palette);
    vp->bg_pattern = viewport_color(theme, UI_COLOR_BACKGROUND, &palette);
    core_put_data_pinned(bar);
}

int sb_visible_height(void)
{
    if (native_wps_active() || !viewportmanager_theme_enabled(SCREEN_MAIN) ||
        status_handle <= 0)
        return 0;
    struct statusbar *bar = core_get_data_pinned(status_handle);
    refresh_theme(bar);
    int height = bar->fonts_ready ? bar->theme.metric[UI_METRIC_STATUS_HEIGHT] : 0;
    core_put_data_pinned(bar);
    return height;
}

void sb_update(enum screen_type screen, bool force)
{
    if (rock_ui_raw() || status_handle <= 0 || screen != SCREEN_MAIN)
        return;

    struct statusbar *bar = core_get_data_pinned(status_handle);
    if (!lcd_active() || native_wps_active() || !viewportmanager_theme_enabled(screen))
    {
        indicators_moving = false;
        bar->motion.ready = false;
        retire_trace(false);
        goto done;
    }
    refresh_theme(bar);
    struct status_title *title = &bar->title[screen];
    unsigned palette_generation = album_palette_generation();
    force |= invalidated || title->dirty || palette_generation != bar->palette_generation;
    force |= refresh_status_colors(bar);
    bool poll = force || !TIME_BEFORE(current_tick, bar->next_update);
    if (!poll && !indicators_moving)
        goto done;

    if (!bar->fonts_tried)
    {
        bar->fonts_ready = native_chrome_open(&bar->chrome, &bar->theme);
        bar->fonts_tried = true;
    }
    if (bar->fonts_ready)
    {
        /* The store is movable between pins. */
        bar->chrome.theme = &bar->theme;
        bool changed = poll && native_chrome_poll(&bar->chrome);
        struct native_chrome_indicators target;
        native_chrome_indicators(&bar->chrome.state, &target);
        bool was_moving = indicators_moving;
        unsigned frames = bar->chrome.state.volume_visible ? 0 :
                          ui_theme_duration(INDICATOR_FRAMES);
        changed |= advance_indicators(&bar->motion, &target, USEC_TIMER,
                                      frames);
        indicators_moving = bar->motion.fade.frames != 0;
        if (indicators_moving)
        {
            motion_start = bar->motion.fade.start_clock;
            motion_duration = bar->motion.fade.frames;
            if (!was_moving && !motion_trace && !bar->chrome.state.volume_visible &&
                !ui_flush_inhibited())
                motion_trace = ui_perf_begin_passive("status-icons");
        }
        else if (was_moving)
            retire_trace(changed || force);
        if (changed || force)
        {
            struct ui_rect area = { 0, 0, LCD_WIDTH,
                bar->theme.metric[UI_METRIC_STATUS_HEIGHT] };
            struct ui_view_frame frame = { .bounds = area, .clip = area,
                .opacity = UI_FULL_OPACITY, .brightness = UI_FULL_OPACITY };
            struct ui_album_palette palette;
            album_palette_snapshot(&palette);
            struct viewport *old = lcd_set_viewport(NULL);
            unsigned int start = USEC_TIMER;
            native_chrome_draw(&bar->chrome, title->text, false, &frame, &palette,
                               &bar->motion.visible, bar->adaptive ? &bar->colors : NULL);
            ui_note_render(USEC_TIMER - start);
            lcd_set_viewport(old);
            ui_mark_dirty_rect(screen, area.x, area.y, area.width, area.height);
        }
    }
    else if (indicators_moving)
    {
        indicators_moving = false;
        retire_trace(false);
    }
    bar->palette_generation = palette_generation;
    if (poll)
        bar->next_update = current_tick + update_delay;
    title->dirty = invalidated = false;
done:
    core_put_data_pinned(bar);
}

void sb_settle(enum screen_type screen) { sb_update(screen, true); }
void sb_invalidate(void) { invalidated = true; }
void sb_set_update_delay(int delay) { update_delay = delay; }

bool sb_background_busy(void)
{
    return tagcache_is_busy() || art_cache_is_busy() || file_index_is_busy();
}

bool sb_show_disk_activity(void)
{
    return global_settings.show_disk_activity && led_read(HZ / 2);
}

bool sb_show_background_tasks(void)
{
    return global_settings.show_background_tasks && sb_background_busy();
}

bool sb_show_working(void)
{
    return global_settings.show_working && ui_working();
}

static void wake_statusbar(void)
{
    static long next_check;
    static bool was_active;
    if (TIME_BEFORE(current_tick, next_check))
        return;
    if (viewportmanager_theme_enabled(SCREEN_MAIN) && lcd_active() &&
        button_queue_empty())
    {
        bool active = indicators_moving || native_chrome_volume_visible() || sb_show_disk_activity() ||
                      sb_show_background_tasks() || sb_show_working();
        unsigned frames = active || was_active ? BUSY_REDRAW_FRAMES : IDLE_REDRAW_FRAMES;
        next_check = current_tick + frames * HZ / UI_ANIM_FPS;
        was_active = active;
        button_queue_post(BUTTON_NONE, 0);
    }
    else
        next_check = current_tick + BUSY_REDRAW_FRAMES * HZ / UI_ANIM_FPS;
}

void sb_init(void)
{
    status_handle = core_alloc_free_space(sizeof(struct statusbar));
    if (status_handle <= 0)
        return;
    struct statusbar *bar = core_get_data_pinned(status_handle);
    memset(bar, 0, sizeof(*bar));
    bar->generation = ~ui_theme_generation();
    refresh_theme(bar);
    core_put_data_pinned(bar);
    tick_add_task(wake_statusbar);
}
