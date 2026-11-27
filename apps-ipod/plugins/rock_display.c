/* Foreground plugin display ownership. GNU GPL version 2 or later. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "plugins/rock_widgets.h"
#include "plugins/rock_compat.h"
#include "button.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "kernel.h"
#include "settings/settings.h"
#include "lang.h"
#include "system/appevents.h"
#include "system/activity.h"
#include "ui/frame.h"
#include "ui/native_wps.h"
#include "ui/retained.h"
#include "ui/scene.h"

static struct rock_display_state caller;
static struct ui_scene app_scene;
static bool app_first_frame;
static fb_data *saved_pixels, *plugin_pixels;
static bool raw, status_update, flush_inhibited;
static unsigned theme_depth, theme_floor;

bool rock_ui_raw(void)
{
    return raw && !status_update;
}

bool rock_ui_native(void)
{
    return saved_pixels && !raw;
}

static void save_display(struct rock_display_state *state)
{
    state->viewport = lcd_set_viewport(NULL);
    state->defaults = *lcd_init_viewport(NULL);
    state->backdrop = lcd_get_backdrop();
    state->persistent = sb_get_title(SCREEN_MAIN, state->title,
                                     sizeof(state->title), &state->icon);
}

static void restore_display(const struct rock_display_state *state)
{
    if (state->persistent)
        sb_set_persistent_title(state->title, state->icon, SCREEN_MAIN);
    else
        sb_set_title_text(state->title, state->icon, SCREEN_MAIN);
    lcd_set_backdrop(state->backdrop);
    *lcd_init_viewport(NULL) = state->defaults;
    lcd_set_viewport(state->viewport);
}

void rock_display_start(fb_data *caller_pixels, fb_data *widget_pixels)
{
    ui_scene_interrupt();
    app_first_frame = get_current_activity() == ACTIVITY_PLUGINBROWSER &&
                      ui_scene_capture_page(&app_scene);
    lcd_scroll_stop();
    save_display(&caller);
    saved_pixels = caller_pixels;
    plugin_pixels = widget_pixels;
    memcpy(saved_pixels, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    flush_inhibited = ui_flush_inhibited();
    ui_inhibit_flush(false);
    ui_take_dirty(SCREEN_MAIN);

    raw = true;
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    sb_set_persistent_title(NULL, Icon_NOICON, SCREEN_MAIN);
    viewport_set_fullscreen(lcd_init_viewport(NULL), SCREEN_MAIN);
    lcd_set_backdrop(NULL);
}

void rock_display_stop(bool back_to_browser)
{
    /* The plugin's current viewport may have lived on its entry stack. */
    *screens[SCREEN_MAIN].current_viewport = lcd_init_viewport(NULL);
    lcd_scroll_stop();
    if (app_first_frame)
        ui_scene_discard(&app_scene);
    else
        ui_scene_close_modal(&app_scene, back_to_browser, "app-pop");
    app_first_frame = false;
    ui_scene_interrupt();
    lcd_set_viewport(NULL);
    while (theme_depth)
        rock_theme_undo(SCREEN_MAIN, false);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    restore_display(&caller);

    raw = false;
    action_handoff_enter();
    memcpy(lcd_framebuffer_default.fb_ptr, saved_pixels, FRAMEBUFFER_SIZE);
    saved_pixels = plugin_pixels = NULL;
    ui_take_dirty(SCREEN_MAIN);
    ui_retained_invalidate();
    native_wps_invalidate();
    sb_invalidate();
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
    ui_inhibit_flush(flush_inhibited);
    button_queue_post(BUTTON_REDRAW, 0);
}

struct rock_native_scope rock_display_native_begin(void)
{
    struct rock_native_scope scope = {
        .from_raw = raw,
        .previous_floor = theme_floor,
    };
    theme_floor = theme_depth;
    save_display(&scope.display);
    if (raw)
    {
        lcd_scroll_stop();
        memcpy(plugin_pixels, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
        raw = false;
        action_handoff_enter();
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        lcd_set_backdrop(caller.backdrop);
        viewport_set_defaults(lcd_init_viewport(NULL), SCREEN_MAIN);
        sb_set_persistent_title(NULL, Icon_NOICON, SCREEN_MAIN);
        sb_invalidate();
    }
    return scope;
}

void rock_display_native_end(const struct rock_native_scope *scope,
                             enum rock_display_resume resume)
{
    while (theme_depth > theme_floor)
    {
        --theme_depth;
        viewportmanager_theme_undo(SCREEN_MAIN, false);
    }
    theme_floor = scope->previous_floor;
    if (scope->from_raw)
    {
        ui_scene_interrupt();
        lcd_scroll_stop();
        viewportmanager_theme_undo(SCREEN_MAIN, true);
        ui_take_dirty(SCREEN_MAIN);
        raw = true;
        if (resume != ROCK_RESUME_DISCARD && !rock_usb_taken())
        {
            memcpy(lcd_framebuffer_default.fb_ptr, plugin_pixels,
                   FRAMEBUFFER_SIZE);
            if (resume == ROCK_RESUME_PRESENT)
            {
                lcd_set_viewport(NULL);
                lcd_update();
            }
        }
    }
    if (resume != ROCK_RESUME_DISCARD)
        restore_display(&scope->display);
}

void rock_theme_enable(enum screen_type screen, bool enable, struct viewport *vp)
{
    rock_list_release(false);
    viewportmanager_theme_enable(screen, enable, vp);
    ++theme_depth;
}

void rock_theme_undo(enum screen_type screen, bool force)
{
    rock_list_release(false);
    if (theme_depth > theme_floor)
    {
        --theme_depth;
        viewportmanager_theme_undo(screen, force);
    }
}

bool rock_set_title(const char *title, enum themable_icons icon,
                    enum screen_type screen)
{
    return sb_set_persistent_title(P2STR((const unsigned char *)title),
                                   icon, screen);
}

void rock_send_event(unsigned short id, void *data)
{
    if (raw && id == GUI_EVENT_ACTIONUPDATE)
    {
        /* A raw plugin can explicitly request native chrome above its viewport. */
        if (theme_depth && viewportmanager_theme_enabled(SCREEN_MAIN))
        {
            status_update = true;
            sb_update(SCREEN_MAIN, false);
            ui_flush_dirty();
            sb_motion_presented();
            status_update = false;
        }
        return;
    }
    send_event(id, data);
}

bool rock_display_first_page(void)
{
    if (!app_first_frame)
        return false;
    app_first_frame = false;
    ui_scene_show_modal(&app_scene, "app-push");
    return app_scene.capturing;
}

/* Raw LCD calls do not pump the native action loop. */
static void rock_present_first_page(void)
{
    if (rock_display_first_page())
        ui_scene_finish_page(&app_scene);
}

void rock_raw_lcd_update(void)
{
    rock_list_release(false);
    rock_present_first_page();
    lcd_update();
}

void rock_raw_lcd_update_rect(int x, int y, int w, int h)
{
    rock_list_release(false);
    rock_present_first_page();
    lcd_update_rect(x, y, w, h);
}

/* A raw call reclaims the framebuffer before it writes any pixels. */
#define ROCK_RAW_VOID(name, args, call) \
    void rock_raw_##name args \
    { \
        rock_list_release(false); \
        name call; \
    }
#define ROCK_RAW_VALUE(type, name, args, call) \
    type rock_raw_##name args \
    { \
        rock_list_release(false); \
        return name call; \
    }
#include "rock_raw_lcd.def"
#undef ROCK_RAW_VOID
#undef ROCK_RAW_VALUE

void rock_raw_lcd_putsxyf(int x, int y, const unsigned char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rock_raw_lcd_putsxy(x, y, buf);
}

void rock_raw_lcd_putsf(int x, int y, const unsigned char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rock_raw_lcd_puts(x, y, buf);
}
