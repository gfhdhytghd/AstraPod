/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The text reel: lines crawling up the whole screen, centred, over a solid
 * fill. Backs the credits list.
 *
 * Parts: backlight and colours, then the scroll loop. The word wrap is
 * draw/text_wrap.c's.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "backlight.h"
#include "powermgmt.h"
#include "system/shutdown.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "draw/text_wrap.h"
#include "text_reel.h"

/* Text centred across the screen, in the theme's foreground and background
 * colours -- or, while dynamic colours are running, the pair the album art is
 * currently lending them. The column is inset from the side edges so a full
 * line of prose does not run into them; the fill still covers the screen. */
#define TR_PAD 10                          /* inset from the side edges      */
#define TR_W   (LCD_WIDTH - 2 * TR_PAD)    /* text column, lines centred     */

/* Preserve the crawl speed on the same clock as the scene transition. */
#define TR_PIXELS_PER_SECOND 50
#define TR_RESUME_FRAMES ANIM_F(60)

static void tr_backlight_ignore_timeout(void)
{
    if (global_settings.backlight_timeout > 0)
        backlight_set_timeout(0);
    if (global_settings.backlight_timeout_plugged > 0)
        backlight_set_timeout_plugged(0);
}

static void tr_backlight_use_settings(void)
{
    backlight_set_timeout(global_settings.backlight_timeout);
    backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
}

struct reel {
    const char *const *lines;
    unsigned char *heights;
    int font, count, pitch, scroll;
};

static void paint_reel(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct reel *reel = view->data;
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = reel->font,
        .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
        .fg_pattern = ui_theme_rgb565(UI_COLOR_FOREGROUND),
        .bg_pattern = ui_theme_rgb565(UI_COLOR_BACKGROUND),
    };
    struct viewport *last = d->set_viewport(&vp);
    d->clear_viewport();

    int y = vp.height - reel->scroll;
    for (int i = 0; i < reel->count && y < vp.height; ++i)
    {
        int height = reel->heights[i] * reel->pitch;
        if (y + height > 0)
            text_wrap_draw(d, reel->lines[i], TR_PAD, y, TR_W, 'c');
        y += height;
    }
    d->set_viewport(last);
}

static const struct ui_view_ops reel_ops = { .paint = paint_reel };

/* Lines read aloud, at most: the credits run to hundreds of names */
#define TR_SPOKEN_MAX 100

int text_reel_run(const char *const *lines, unsigned char *heights, int count)
{
    const struct ui_rect bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    struct reel reel = { .lines = lines, .heights = heights, .count = count,
                         .font = screens[SCREEN_MAIN].getuifont() };
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int total_lines = 0;
    bool manual = false;
    unsigned last_pixels = 0;
    int ret = 0;

    tr_backlight_ignore_timeout();
    ui_scene_open_page(&scene, "credits-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    font_getstringsize((const unsigned char *)"A", NULL, &reel.pitch, reel.font);
    ui_view_init(&view, bounds, &reel_ops, &reel);
    ui_page_init(&page, &view);

    /* Measure how tall each line is once, up front. */
    for (int i = 0; i < count; i++)
    {
        /* an empty line still takes one: it is the gap between sections */
        heights[i] = MAX(text_wrap_count(lines[i], TR_W, reel.font), 1);
        total_lines += heights[i];
    }

    /* Line 0 enters from the bottom (top == LCD_HEIGHT at scroll 0); at
     * end_scroll the last line has cleared the top and the screen exits. */
    int end_scroll = LCD_HEIGHT + total_lines * reel.pitch;
    ui_page_draw(&page);

    /* Read out a line at a time, as far as the speech queue reaches */
    if (global_settings.talk_menu)
    {
        talk_force_shutup();
        for (int i = 0; i < count && i < TR_SPOKEN_MAX; i++)
            if (lines[i][0])
                talk_text_part(lines[i]);
    }

    uint32_t scroll_clock = ui_clock_start();
    while (1)
    {
        uint32_t elapsed = USEC_TIMER - scroll_clock;
        unsigned until = manual ? TR_RESUME_FRAMES : ui_clock_frame(elapsed) + 1;
        int action = get_action(CONTEXT_LIST, ui_clock_timeout(elapsed, until, HZ));

        switch (action)
        {
            case ACTION_STD_CANCEL:
            case ACTION_STD_MENU:
                goto done;

            case ACTION_STD_NEXT:        /* wheel forward: nudge the reel up   */
            case ACTION_STD_NEXTREPEAT:
                manual = true;
                scroll_clock = ui_clock_start();
                last_pixels = 0;
                reel.scroll += reel.pitch;
                break;

            case ACTION_STD_PREV:        /* wheel back: nudge the reel down    */
            case ACTION_STD_PREVREPEAT:
                manual = true;
                scroll_clock = ui_clock_start();
                last_pixels = 0;
                reel.scroll -= reel.pitch;
                break;

            case ACTION_NONE:
            {
                unsigned frame = ui_clock_frame(USEC_TIMER - scroll_clock);
                if (manual && frame >= TR_RESUME_FRAMES)
                {
                    manual = false;
                    scroll_clock = ui_clock_start();
                }
                else if (!manual)
                {
                    unsigned pixels = (uint64_t)ui_frame_clock(frame) *
                                      TR_PIXELS_PER_SECOND / 1000000;
                    reel.scroll += pixels - last_pixels;
                    last_pixels = pixels;
                }
                break;
            }

            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = SYS_USB_CONNECTED;
                    goto done;
                }
                break;
        }

        reel.scroll = MAX(0, MIN(reel.scroll, end_scroll));

        /* Manual scrolling parks at the end until the reel resumes. */
        if (!manual && reel.scroll >= end_scroll)
            goto done;

        ui_page_invalidate(&page, bounds);
        ui_page_draw(&page);
        reset_poweroff_timer();
    }

  done:
    talk_shutup();
    ui_scene_close_modal(&scene, ret != SYS_USB_CONNECTED, "credits-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    tr_backlight_use_settings();
    return ret;
}
