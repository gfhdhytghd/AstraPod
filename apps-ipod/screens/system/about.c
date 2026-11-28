/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The About page: the logo, a few lines on what this firmware is and its
 * version, and a QR code to the project page, all in the theme's colours.
 *
 * Parts: the QR code, the page, then the loop.
 ****************************************************************************/

#include "config.h"
#include "kernel.h"
#include "system.h"
#include "lang.h"
#include "lcd.h"
#include "version.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "draw/text_wrap.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "speech/talk.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/menu.h"
#include "about_art.h"
#include "about.h"
#include "fruit_about.h"

#define AB_PAD     16                    /* from the screen's edges         */
#define AB_GAP     12                    /* between the text and the code   */
#define AB_QR_PX   3                     /* pixels to a module              */
#define AB_QR_QUIET 4                    /* modules of light border         */
#define AB_QR_SIZE ((ABOUT_QR_N + 2 * AB_QR_QUIET) * AB_QR_PX)
#define AB_TEXT_W  (LCD_WIDTH - 2 * AB_PAD - AB_GAP - AB_QR_SIZE)

static const char ab_tagline[] =
    "Rockbox specifically and deeply optimized for iPod Classic.";
static const char ab_licence[] = "GPL v2, no warranty.";

static void ab_draw_logo(struct screen *d, struct viewport *vp, int x, int y)
{
    unsigned fg = vp->fg_pattern;

    vp->fg_pattern = LCD_WHITE;
    d->alpha_bitmap_part(about_logo_white, 0, 0, ABOUT_LOGO_W,
                         x, y, ABOUT_LOGO_W, ABOUT_LOGO_H);
    vp->fg_pattern = ABOUT_LOGO_ACCENT;
    d->alpha_bitmap_part(about_logo_accent, 0, 0, ABOUT_LOGO_W,
                         x, y, ABOUT_LOGO_W, ABOUT_LOGO_H);
    d->alpha_bitmap_part_img(about_mark, about_mark_alpha, 0, 0,
                             x + ABOUT_MARK_X, y + ABOUT_MARK_Y,
                             ABOUT_MARK_N, ABOUT_MARK_N,
                             ABOUT_MARK_N, ABOUT_MARK_N);
    vp->fg_pattern = fg;
}

void about_draw_boot_logo(struct screen *display, struct viewport *vp, int bottom)
{
    ab_draw_logo(display, vp, (display->lcdwidth - ABOUT_LOGO_W) / 2,
                 bottom - ABOUT_LOGO_H);
}

static int ab_luma(unsigned c)
{
    return RGB_UNPACK_RED(c) * 77 + RGB_UNPACK_GREEN(c) * 150
           + RGB_UNPACK_BLUE(c) * 29;
}

/* Dark modules on a light square, whichever of the pair is which: a scanner
 * expects that polarity, and a light-on-dark theme would otherwise invert it. */
static void ab_draw_qr(struct screen *d, struct viewport *vp, int x, int y)
{
    unsigned fg = vp->fg_pattern, bg = vp->bg_pattern;
    unsigned light = ab_luma(fg) > ab_luma(bg) ? fg : bg;
    int stride = (ABOUT_QR_N + 7) / 8;

    vp->fg_pattern = light;
    d->fillrect(x, y, AB_QR_SIZE, AB_QR_SIZE);
    vp->fg_pattern = light == fg ? bg : fg;
    x += AB_QR_QUIET * AB_QR_PX;
    y += AB_QR_QUIET * AB_QR_PX;
    for (int r = 0; r < ABOUT_QR_N; r++)
        for (int c = 0; c < ABOUT_QR_N; c++)
            if (about_qr[r * stride + c / 8] & (0x80 >> (c % 8)))
                d->fillrect(x + c * AB_QR_PX, y + r * AB_QR_PX,
                            AB_QR_PX, AB_QR_PX);
    vp->fg_pattern = fg;
}

/* The logo at the top left, then the text beside the code, with the screen's
 * spare height split evenly above, between and below the two. */
static void ab_draw(struct screen *d, struct viewport *vp,
                    const char *const *text, int count, int line_h)
{
    int text_h = (count - 1) * line_h / 2;   /* half a line between items */
    int block, gap, top, y;

    for (int i = 0; i < count; i++)
        text_h += text_wrap_count(text[i], AB_TEXT_W, vp->font) * line_h;
    block = MAX(text_h, AB_QR_SIZE);
    gap = MAX((LCD_HEIGHT - ABOUT_LOGO_H - block) / 3, 0);
    top = 2 * gap + ABOUT_LOGO_H;

    d->clear_viewport();
    ab_draw_logo(d, vp, AB_PAD, gap);
    ab_draw_qr(d, vp, LCD_WIDTH - AB_PAD - AB_QR_SIZE,
               top + (block - AB_QR_SIZE) / 2);

    y = top + (block - text_h) / 2;
    for (int i = 0; i < count; i++)
        y += text_wrap_draw(d, text[i], AB_PAD, y, AB_TEXT_W, 'l') * line_h
             + line_h / 2;
}

struct about_page {
    const char *text[3];
    unsigned fg, bg;
};

static void about_paint(const struct ui_view *view,
                        const struct ui_view_frame *frame)
{
    const struct about_page *about = view->data;
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport vp = {
        .x = frame->bounds.x, .y = frame->bounds.y,
        .width = frame->bounds.width, .height = frame->bounds.height,
        .flags = VP_DEFAULT_FLAGS, .font = d->getuifont(),
        .drawmode = DRMODE_FG, .buffer = &lcd_framebuffer_default,
        .fg_pattern = about->fg, .bg_pattern = about->bg,
    };
    struct viewport *last = d->set_viewport(&vp);
    int line_h;

    d->getstringsize((const unsigned char *)"A", NULL, &line_h);
    ab_draw(d, &vp, about->text, ARRAYLEN(about->text), line_h);
    d->set_viewport(last);
}

static const struct ui_view_ops about_ops = { .paint = about_paint };

int about_screen(void)
{
    if (stock_art_ready())
        return fruit_about_screen();

    const struct ui_rect bounds = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    struct about_page about = { .text = {ab_tagline, rbversion, ab_licence} };
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    bool drawn = false;
    int ret = 0;

    ui_view_init(&view, bounds, &about_ops, &about);
    ui_page_init(&page, &view);
    ui_scene_open_page(&scene, "about-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);

    if (global_settings.talk_menu)
    {
        talk_force_shutup();
        talk_text_part(ab_tagline);
        talk_id(LANG_VERSION, true);
        talk_spell(rbversion, true);
        talk_text_part(ab_licence);
    }

    while (1)
    {
        /* Redrawn only when the colours move: they follow the album palette,
         * which fades between tracks. */
        unsigned fg = ui_theme_rgb565(UI_COLOR_FOREGROUND);
        unsigned bg = ui_theme_rgb565(UI_COLOR_BACKGROUND);
        if (!drawn || about.fg != fg || about.bg != bg)
        {
            drawn = true;
            about.fg = fg;
            about.bg = bg;
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
        }

        int action = get_action(CONTEXT_STD, HZ / 10);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU
            || action == ACTION_STD_OK)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = MENU_ATTACHED_USB;
            break;
        }
    }

    talk_shutup();
    ui_scene_close_modal(&scene, ret != MENU_ATTACHED_USB, "about-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}
