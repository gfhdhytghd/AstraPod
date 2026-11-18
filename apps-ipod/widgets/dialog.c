/***************************************************************************
 * Modal box primitive shared by the popup, yes/no and text-input dialogs.
 * Draws a centred bordered box and returns its content viewport; owns the
 * dialog style defaults.
 ****************************************************************************/

#include "config.h"
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "backlight.h"
#include "button.h"
#include "draw/icon.h"
#include "draw/color.h"
#include "draw/round_rect.h"
#include "draw/text_wrap.h"
#include "metadata/album_palette.h"
#include "settings/settings.h"
#include "dialog.h"
#include "core_alloc.h"
#include "system.h"
#include "thread.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/sheet.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include <string.h>
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif

static unsigned theme_color(const struct ui_theme *theme, enum ui_color_role role,
                            const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

void dialog_style_default(struct dialog_style *s)
{
    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    const uint16_t *m = theme.metric;

    unsigned bg = theme_color(&theme, UI_COLOR_MODAL_BG, &palette);
    unsigned fg = theme_color(&theme, UI_COLOR_MODAL_FG, &palette);
    unsigned outline = theme_color(&theme, UI_COLOR_MODAL_BORDER, &palette);
    unsigned accent = theme_color(&theme, UI_COLOR_MODAL_ACCENT, &palette);
    *s = (struct dialog_style){
        .box_fg = fg,
        .box_bg = bg,
        .box_border_color = outline,
        .box_border_width = m[UI_METRIC_MODAL_BORDER_WIDTH],
        .box_margin = m[UI_METRIC_MODAL_PADDING],
        .box_font = DIALOG_FONT_INHERIT,
        .edge_margin = m[UI_METRIC_MODAL_EDGE_MARGIN],
        .button_fg = fg,
        .button_bg = bg,
        .button_border_color = outline,
        .button_fg_selected = theme_color(&theme, UI_COLOR_MODAL_SELECTED_TEXT, &palette),
        .button_bg_selected = accent,
        .button_border_color_selected = accent,
        .button_border_width = m[UI_METRIC_MODAL_BORDER_WIDTH],
        .button_font = DIALOG_FONT_INHERIT,
        .button_gap = m[UI_METRIC_MODAL_BUTTON_GAP],
        .button_pad_x = m[UI_METRIC_MODAL_BUTTON_PAD_X],
        .button_pad_y = m[UI_METRIC_MODAL_BUTTON_PAD_Y],
    };
}

void dialog_get_insets(const struct dialog_style *style,
                       struct dialog_insets *out)
{
    out->left = out->top = out->right = out->bottom = style->box_margin;
}

/* ---------------------------------------------------------------------- *
 * Frame primitive                                                        *
 *                                                                        *
 * The box itself is always square and uses the plain rectangle pair       *
 * below. Only dialog_draw_button() rounds, by the theme's                 *
 * `dialog button border radius`, via draw/round_rect.c.                   *
 * ---------------------------------------------------------------------- */

/* Square border, `bw` pixels thick, in the drawmode/foreground already set.
 * One fillrect per side rather than nested outlines, so every side is `bw`
 * by construction and there is nothing to get out of step. */
static void draw_box_border(struct screen *s, int w, int h, int bw)
{
    if (bw <= 0)
        return;
    if (bw * 2 > h)                     /* a border thicker than the box */
        bw = h / 2;

    s->fillrect(0, 0, w, bw);                        /* top    */
    s->fillrect(0, h - bw, w, bw);                   /* bottom */
    s->fillrect(0, bw, bw, h - 2 * bw);              /* left   */
    s->fillrect(w - bw, bw, bw, h - 2 * bw);         /* right  */
}

static unsigned resolve_fg(struct screen *s, unsigned color)
{
    return color == DIALOG_COLOR_INHERIT ? s->get_foreground() : color;
}

static unsigned resolve_bg(struct screen *s, unsigned color)
{
    return color == DIALOG_COLOR_INHERIT ? s->get_background() : color;
}

void dialog_frame_box(struct screen *screen, struct viewport *box,
                      const struct dialog_style *style,
                      struct viewport *content_out)
{
    int bw = MIN(style->box_border_width, box->width / 2);
    struct dialog_insets in;

    if (screen->depth > 1)
    {
        unsigned fg = resolve_fg(screen, style->box_fg);
        unsigned bg = resolve_bg(screen, style->box_bg);
        unsigned bc = resolve_fg(screen, style->box_border_color);
        screen->set_drawmode(DRMODE_FG);
        screen->set_foreground(bg);          /* fill with the background colour */
        screen->fillrect(0, 0, box->width, box->height);
        screen->set_foreground(bc);
        draw_box_border(screen, box->width, box->height, bw);
        screen->set_foreground(fg);          /* interior draws in the box fg */
        screen->set_drawmode(DRMODE_SOLID);

        box->fg_pattern = fg;
        box->bg_pattern = bg;
    }
    else
    {
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
        screen->fillrect(0, 0, box->width, box->height);
        screen->set_drawmode(DRMODE_SOLID);
        draw_box_border(screen, box->width, box->height, bw);
    }

    /* content sub-viewport: the box inset by the margin. It inherits the box's
     * (styled) colours and font. */
    dialog_get_insets(style, &in);
    *content_out = *box;
    content_out->x      += in.left;
    content_out->y      += in.top;
    content_out->width  -= in.left + in.right;
    content_out->height -= in.top + in.bottom;
}

#define BTN_PAD_X    4   /* gap from the border to the icon or label */
#define BTN_ICON_GAP 4   /* gap between the icon and the label */

/* Draw the icon and label inside a button already filled and bordered.
 *
 * The label goes into a sub-viewport covering only the button's interior, so
 * it is clipped rather than drawn over the border -- a bare putsxy() of a label
 * wider than the button spills out of it in both directions, which is what a
 * list row does all the time and a Cancel/OK pair never does.
 *
 * `pad_left` already includes the icon, so the sub-viewport starts past it. */
static void draw_button_content(struct screen *screen, int x, int y,
                                int w, int h, int lw, int lh,
                                const char *label,
                                enum dialog_button_align align,
                                const struct bitmap *icon,
                                int scroll_px, int *overflow_out,
                                bool selected, bool colour,
                                unsigned fill, unsigned text)
{
    struct viewport *cur = *screen->current_viewport;
    struct viewport tvp;
    struct viewport *prev;
    int icon_w = icon ? icon->width + BTN_ICON_GAP : 0;
    int overflow, tx = 0;

    if (overflow_out)
        *overflow_out = 0;

    if (icon)
    {
        /* mono_bitmap paints set bits in the current foreground, so the icon
         * takes the label's colour and needs no palette of its own. */
        screen->set_drawmode(selected && !colour ? DRMODE_SOLID | DRMODE_INVERSEVID
                                                 : DRMODE_FG);
        if (colour)
            screen->set_foreground(text);
        screen->mono_bitmap(icon->data, x + BTN_PAD_X,
                            y + (h - icon->height) / 2,
                            icon->width, icon->height);
        screen->set_drawmode(DRMODE_SOLID);
    }

    /* A sub-viewport covering just the label's space, so putsxy() below is
     * clipped to it however wide the label is. */
    tvp = *cur;                        /* flags and buffer come with it */
    tvp.x = cur->x + x + BTN_PAD_X + icon_w;
    tvp.y = cur->y + y + (h - lh) / 2;
    tvp.width = w - 2 * BTN_PAD_X - icon_w;
    tvp.height = lh;
    if (tvp.width < 1 || tvp.height < 1)
        return;

    if (colour)
    {
        tvp.fg_pattern = text;
        tvp.bg_pattern = fill;
    }

    overflow = lw - tvp.width;
    if (overflow < 0)
        overflow = 0;
    if (overflow_out)
        *overflow_out = overflow;

    if (scroll_px > overflow)
        scroll_px = overflow;
    if (scroll_px < 0)
        scroll_px = 0;

    prev = screen->set_viewport(&tvp);

    if (overflow == 0 && align == DIALOG_BTN_CENTRE)
        tx = (tvp.width - lw) / 2;
    else
        tx = -scroll_px;

    screen->set_drawmode(selected && !colour ? DRMODE_SOLID | DRMODE_INVERSEVID
                                             : DRMODE_FG);
    screen->putsxy(tx, 0, (const unsigned char *)label);

    screen->set_drawmode(DRMODE_SOLID);
    screen->set_viewport(prev);
}

void dialog_draw_button_ex(struct screen *screen,
                           const struct dialog_style *style,
                           int x, int y, int w, int h,
                           const char *label, bool selected,
                           enum dialog_button_align align,
                           const struct bitmap *icon,
                           int scroll_px, int *overflow_out)
{
    int r  = style->button_border_radius;
    int bw = style->button_border_width;
    int old_font = (*screen->current_viewport)->font;
    bool colour = screen->depth > 1;
    unsigned fill = 0, text = 0, box_fg = 0;
    int lw, lh;

    if (style->button_font != DIALOG_FONT_INHERIT)
        screen->setfont(style->button_font);

    screen->getstringsize((const unsigned char *)label, &lw, &lh);

    if (colour)
    {
        /* Inheriting reproduces the old inverse-video selector exactly: the
         * selected button takes the box's foreground as its fill and its
         * background as its text, and the unselected one fills with the box's
         * own background, so it reads as the plain outline drawn before. */
        unsigned border;

        box_fg = screen->get_foreground();

        fill   = selected ? resolve_fg(screen, style->button_bg_selected)
                          : resolve_bg(screen, style->button_bg);
        text   = selected ? resolve_bg(screen, style->button_fg_selected)
                          : resolve_fg(screen, style->button_fg);
        border = selected ? resolve_fg(screen, style->button_border_color_selected)
                          : resolve_fg(screen, style->button_border_color);

        screen->set_drawmode(DRMODE_FG);
        screen->set_foreground(fill);
        fill_round_rect(screen, x, y, w, h, r);
        screen->set_foreground(border);
        draw_round_rect(screen, x, y, w, h, r, bw);
    }
    else if (selected)
    {
        screen->set_drawmode(DRMODE_FG);
        fill_round_rect(screen, x, y, w, h, r);
    }
    else
    {
        screen->set_drawmode(DRMODE_SOLID);
        draw_round_rect(screen, x, y, w, h, r, bw);
    }

    draw_button_content(screen, x, y, w, h, lw, lh, label, align, icon,
                        scroll_px, overflow_out, selected, colour, fill, text);

    if (colour)
        screen->set_foreground(box_fg);      /* restore for the rest of the box */
    screen->set_drawmode(DRMODE_SOLID);

    if (style->button_font != DIALOG_FONT_INHERIT)
        screen->setfont(old_font);
}

void dialog_draw_button(struct screen *screen,
                        const struct dialog_style *style,
                        int x, int y, int w, int h,
                        const char *label, bool selected)
{
    dialog_draw_button_ex(screen, style, x, y, w, h, label, selected,
                          DIALOG_BTN_CENTRE, NULL, 0, NULL);
}

#define DIALOG_TITLE_GAP 4       /* either side of the rule under a title */

int dialog_title_height(const char *title)
{
    if (!title)
        return 0;
    return font_get(font_get_ui_bold())->height + 2 * DIALOG_TITLE_GAP + 1;
}

void dialog_draw_title(struct screen *screen, struct viewport *content,
                       const char *title)
{
    int body_font = content->font, w, h;
    if (!title)
        return;
    screen->set_drawmode(DRMODE_FG);
    content->font = font_get_ui_bold();
    screen->getstringsize(title, &w, &h);
    screen->putsxy(MAX((content->width - w) / 2, 0), 0, title);
    content->font = body_font;
    screen->hline(0, content->width - 1,
                  dialog_title_height(title) - DIALOG_TITLE_GAP - 1);
}

int dialog_wrap_text(const char *text, int max_width, int font,
                     struct dialog_text_line *out, int max_lines)
{
    struct font *pf = font_get(font);
    int count = 0;

    font_lock(font, true);
    while (*text && count < max_lines)
    {
        const char *end;

        out[count].str = text;
        text = text_wrap_next(text, max_width, pf, &end);
        out[count].len = (int)(end - out[count].str);
        count++;
    }
    font_lock(font, false);

    return count;
}

void dialog_init(struct dialog *d, int context, const char *title,
                 const struct dialog_style *style,
                 const struct dialog_callbacks *cb, void *data)
{
    *d = (struct dialog){
        .cb = cb,
        .data = data,
        .context = context,
        .title = title,
        .backlight_on = true,
        .default_style = !style,
    };
    if (style)
        d->style = *style;
    else
        dialog_style_default(&d->style);
}

/* Decoration, text and custom content use this painter both in a screen
 * viewport and in a retained surface. Presentation belongs to the caller. */
static void dialog_draw_content(struct dialog *d, struct screen *screen,
                                struct viewport *box)
{
    struct viewport *previous = *screen->current_viewport;
    screen->set_viewport_ex(box, VP_FLAG_VP_SET_CLEAN);
    struct viewport content = *box;
    if (d->cb->measure)
        dialog_frame_box(screen, box, &d->style, &content);
    screen->set_viewport_ex(&content, VP_FLAG_VP_SET_CLEAN);
    d->cb->draw(d, screen, &content, d->data);
    screen->set_viewport(previous);
}

static struct viewport dialog_measure(struct dialog *d)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport box = d->parent[SCREEN_MAIN];

    if (d->style.box_font != DIALOG_FONT_INHERIT)
        box.font = d->style.box_font;
    if (d->fb)
    {
        box.buffer = d->fb;
        box.x = box.y = 0;
        box.width = d->fb_w;
        box.height = d->fb_h;
    }
    if (d->cb->measure)
    {
        struct viewport *previous = screen->set_viewport(&box);
        d->cb->measure(d, screen, &box, d->data);
        screen->set_viewport(previous);
    }
    return box;
}

#define PANEL_PIXELS (LCD_WIDTH * LCD_HEIGHT)

struct dialog_view
{
    struct ui_sheet sheet;
    struct ui_rect bounds;
    struct dialog *dialog;
    struct dialog_view *parent;
    unsigned int thread;
    int handle;
    bool interrupted;
    fb_data pixels[];
};

static struct dialog_view *active_dialog;

static fb_data *dialog_output(const struct dialog *d)
{
    return d->fb ? d->fb->fb_ptr : lcd_framebuffer_default.fb_ptr;
}

static void dialog_capture(struct dialog_view *view)
{
    memcpy(view->pixels, dialog_output(view->dialog), FRAMEBUFFER_SIZE);
    view->interrupted = false;
    view->sheet.light = UI_FULL_OPACITY;
}

static struct dialog_view *dialog_view_open(struct dialog *d)
{
    if (d->fb && (d->fb_w <= 0 || d->fb_h <= 0 || d->fb->stride != d->fb_w ||
                  d->fb_w * d->fb_h != PANEL_PIXELS || !d->present))
        return NULL;
    int handle = core_alloc_free_space(sizeof(struct dialog_view) + 2 * FRAMEBUFFER_SIZE);
    if (handle <= 0)
        return NULL;

    struct dialog_view *view = core_get_data_pinned(handle);
    *view = (struct dialog_view){
        .handle = handle,
        .dialog = d,
        .parent = active_dialog,
        .thread = thread_self(),
    };
    dialog_capture(view);
    active_dialog = view;
    return view;
}

static void dialog_view_restore(struct dialog_view *view)
{
    ui_sheet_cancel(&view->sheet);
    if (!view->interrupted)
    {
        struct dialog *d = view->dialog;
        memcpy(dialog_output(d), view->pixels, FRAMEBUFFER_SIZE);
        if (d->fb)
            d->present();
        else
        {
            ui_mark_dirty(SCREEN_MAIN);
            ui_flush_dirty();
        }
    }
}

void dialog_interrupt_transition(void)
{
    if (active_dialog && active_dialog->thread == thread_self())
    {
        dialog_view_restore(active_dialog);
        active_dialog->interrupted = true;
    }
}

static void dialog_submit_direct(struct dialog *d, struct viewport *box)
{
    dialog_draw_content(d, &screens[SCREEN_MAIN], box);
    if (d->fb)
        d->present();
    else
    {
        ui_mark_dirty_rect(SCREEN_MAIN, box->x, box->y, box->width, box->height);
        ui_flush_dirty();
    }
}

static void dialog_view_draw(struct dialog_view *view, struct dialog *d, bool redraw)
{
    if (view->interrupted)
    {
        dialog_capture(view);
        redraw = true;
    }
    if (redraw)
    {
        uint32_t start = USEC_TIMER;
        struct viewport box = dialog_measure(d);
        if (box.width <= 0 || box.height <= 0 ||
            box.width > (d->fb ? d->fb_w : LCD_WIDTH) ||
            box.height > (d->fb ? d->fb_h : LCD_HEIGHT))
        {
            if (view->bounds.width > 0)
                dialog_view_restore(view);
            ui_sheet_cancel(&view->sheet);
            view->bounds = (struct ui_rect){0};
            return;
        }

        struct frame_buffer_t buffer = {
            .fb_ptr = view->pixels + PANEL_PIXELS,
            .stride = box.width,
            .elems = (size_t)box.width * box.height,
        };
        view->bounds = (struct ui_rect){box.x, box.y, box.width, box.height};
        box.x = box.y = 0;
        box.buffer = &buffer;
        dialog_draw_content(d, &screens[SCREEN_MAIN], &box);
        ui_note_render((uint32_t)USEC_TIMER - start);
    }
    if (d->fb)
        ui_sheet_paint_in(&view->sheet, view->pixels, view->pixels + PANEL_PIXELS,
                          view->bounds, redraw, dialog_output(d), d->fb_w, d->fb_h, d->present);
    else
        ui_sheet_paint(&view->sheet, view->pixels, view->pixels + PANEL_PIXELS,
                   view->bounds, redraw);
}

static void dialog_view_dismiss(struct dialog_view *view, struct dialog *d)
{
    if (view->interrupted || view->bounds.width <= 0)
        return;
    ui_sheet_begin(&view->sheet, false, d->motion_route);
    do {
        dialog_view_draw(view, d, false);
    } while (ui_sheet_wait(&view->sheet));
    if (view->sheet.active)
        dialog_view_restore(view);
}

int dialog_run(struct dialog *d, int poll_ticks)
{
    int disp = DIALOG_CONTINUE;
    int font = -1;
    bool outer_inhibit = ui_flush_inhibited();

    ui_inhibit_flush(false);
    ui_scene_interrupt();
    ui_hold_scrolling(true);
    screens[SCREEN_MAIN].scroll_stop();
    viewport_set_defaults(&d->parent[SCREEN_MAIN], SCREEN_MAIN);
    if (d->style.box_font == DIALOG_FONT_INHERIT)
    {
        struct ui_theme theme;
        ui_theme_snapshot(&theme);
        font = ui_theme_font_load(&theme, UI_FONT_BODY, 64);
        if (font >= 0)
            d->parent[SCREEN_MAIN].font = font;
    }

    char title[MAX_PATH];
    enum themable_icons icon = Icon_NOICON;
    bool has_title = sb_get_persistent_title(SCREEN_MAIN, title, sizeof(title), &icon);
    if (d->title && !d->fb)
    {
        sb_set_persistent_title(d->title, Icon_NOICON, SCREEN_MAIN);
        sb_settle(SCREEN_MAIN);
    }
    struct dialog_view *view = dialog_view_open(d);
    d->poll_ticks = poll_ticks;
    /* Wheel events have no release. A USB popup opened after turning the
     * wheel must accept the next key instead of waiting for one. */
    if (button_status() & ~(BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD))
        action_wait_for_release_preserve_queue();

    if (!d->motion_route)
        d->motion_route = d->context == CONTEXT_KEYBOARD ? "keyboard" : "dialog";
    if (view)
        ui_sheet_begin(&view->sheet, true, d->motion_route);

    bool redraw = true;
    int ticks = d->poll_ticks;
    while (disp == DIALOG_CONTINUE)
    {
        unsigned palette_before = album_palette_generation();
        album_palette_refresh();
        redraw |= palette_before != album_palette_generation();
        if (redraw && d->default_style)
            dialog_style_default(&d->style);
        if (view)
            dialog_view_draw(view, d, redraw);
        else
        {
            struct viewport box = dialog_measure(d);
            if (box.width > 0 && box.height > 0)
                dialog_submit_direct(d, &box);
        }

        d->backlight_on = is_backlight_on(false);
        if (album_palette_needs_repaint() && (ticks == TIMEOUT_BLOCK || ticks > HZ / 10))
            ticks = HZ / 10;
        int wake = view ? ui_sheet_timeout(&view->sheet, ticks) : ticks;
        long before = current_tick;
        bool was_inhibited = ui_flush_inhibited();
        ui_inhibit_flush(true);
        int action = d->keymap ? get_custom_action(CONTEXT_PLUGIN, wake, d->keymap)
                               : get_action(d->context, wake);
        ui_inhibit_flush(was_inhibited);

        if (view && ui_take_dirty(SCREEN_MAIN))
            view->sheet.light = 0;
        /* The animation wakes just the compositor. Preserve the dialog's
         * own poll frequency for timeouts, speech, and text scrolling. */
        long elapsed = current_tick - before;
        if (view && wake != ticks && action == ACTION_NONE && (ticks < 0 || elapsed < ticks))
        {
            if (ticks > 0)
                ticks -= elapsed;
            redraw = false;
            continue;
        }
        redraw = true;

#ifdef HAVE_WHEEL_WPS_CHORD
        if (d->context == CONTEXT_KEYBOARD && agents_nav_pending() == AGENTS_NAV_WPS)
        {
            dialog_interrupt_transition();
            disp = DIALOG_CANCEL;
        }
        else
#endif
            disp = d->cb->on_action(d, action, d->data);
        ticks = d->poll_ticks;
    }

    if (view)
    {
        if (disp != DIALOG_ABORT)
            dialog_view_dismiss(view, d);
        else
            dialog_view_restore(view);
        ui_sheet_cancel(&view->sheet);
        active_dialog = view->parent;
        int handle = view->handle;
        core_put_data_pinned(view);
        core_free(handle);
    }
    if (d->title && !d->fb)
    {
        sb_set_persistent_title(has_title ? title : NULL, icon, SCREEN_MAIN);
        if (disp != DIALOG_ABORT)
        {
            sb_settle(SCREEN_MAIN);
            ui_flush_dirty();
        }
    }
    if (d->cb->on_close)
        d->cb->on_close(d, d->data);
    font_unload(font);
    if (ui_hold_scrolling(false))
        sb_invalidate();
    ui_inhibit_flush(outer_inhibit);
    return disp;
}
