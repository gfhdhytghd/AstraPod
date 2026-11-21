/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "font.h"
#include "lang.h"
#include "settings/settings.h"
#include "podlink_screen.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"

struct diagnostic_page {
    struct ui_page page;
    struct ui_view view;
    struct podlink_diagnostics status;
};

static void paint(const struct ui_view *view, const struct ui_view_frame *frame)
{
    static const unsigned char *const names[] = {
        [PODLINK_CLOSED] = "Closed",
        [PODLINK_WAIT_USB] = "Waiting: plug USB in",
        [PODLINK_OPENING] = "USB up / waiting OPEN",
        [PODLINK_CONNECTED] = "Open / messages",
        [PODLINK_CLOSING] = "Closing",
    };
    const struct podlink_diagnostics *status = view->data;
    struct viewport viewport = {
        .x = frame->clip.x, .y = frame->clip.y,
        .width = frame->clip.width, .height = frame->clip.height,
        .flags = VP_DEFAULT_FLAGS,
        .font = FONT_UI,
        .drawmode = DRMODE_SOLID,
        .buffer = &lcd_framebuffer_default,
        .fg_pattern = ui_theme_rgb565(UI_COLOR_FOREGROUND),
        .bg_pattern = ui_theme_rgb565(UI_COLOR_BACKGROUND),
    };
    struct viewport *previous = lcd_set_viewport(&viewport);
    const int x = frame->bounds.x - frame->clip.x;
    const int y = frame->bounds.y - frame->clip.y;
    const int row = font_get(FONT_UI)->height;

    lcd_clear_viewport();
    lcd_putsxy(x, y, str(LANG_PODLINK_DIAGNOSTICS));
    lcd_putsxy(x, y + 2 * row, names[status->state]);
    lcd_putsxyf(x, y + 4 * row, "RX: %lu  TX: %lu", status->received, status->sent);
    lcd_putsxyf(x, y + 5 * row, "Close events: %lu", status->closes);
    lcd_putsxy(x, y + 7 * row, "MENU: close and exit");
    lcd_set_viewport(previous);
}

static const struct ui_view_ops view_ops = { .paint = paint };
static const struct ui_rect bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT};

static void draw(const struct podlink_diagnostics *status, void *context)
{
    struct diagnostic_page *diagnostics = context;
    diagnostics->status = *status;
    ui_page_invalidate(&diagnostics->page, bounds);
    ui_page_draw(&diagnostics->page);
}

int podlink_screen_with_ready(void (*ready)(void))
{
    struct diagnostic_page diagnostics;
    ui_view_init(&diagnostics.view, bounds, &view_ops, &diagnostics.status);
    ui_page_init(&diagnostics.page, &diagnostics.view);
    return podlink_screen_run(draw, &diagnostics, ready);
}

int podlink_screen(void)
{
    return podlink_screen_with_ready(NULL);
}
