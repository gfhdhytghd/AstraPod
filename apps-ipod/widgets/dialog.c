/***************************************************************************
 * Modal box primitive shared by the popup, yes/no and text-input dialogs.
 * Draws a centred bordered box and returns its content viewport; owns the
 * dialog style defaults.
 ****************************************************************************/

/* Modal box primitive: a framed box (dialog_frame_box) plus the interactive
 * theme + skin-flush-guard + get_action loop (dialog_run) shared by the
 * yes/no and text-input dialogs. */

#include "config.h"
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "backlight.h"
#include "draw/icon.h"          /* Icon_NOICON */
#include "draw/color.h"         /* color_blend, color_contrast, tone */
#include "draw/round_rect.h"    /* fill_round_rect/draw_round_rect */
#include "draw/text_wrap.h"
#include "settings/settings.h"  /* theme fg/bg for the derived colours */
#include "skin/statusbar_skinned.h"
#include "skin/skin_engine.h" /* skin_inhibit_flush */
#include "skin/skin_albumart_color.h" /* dynamic_colors_* */
#include "dialog.h"
#include "button_chord.h"
#if defined(IPOD_6G) && (defined(HAVE_USB_PODLINK) || defined(SIMULATOR))
#define DIALOG_SLIDES
#include "button.h"
#include "core_alloc.h"
#include "system.h"
#include "thread.h"
#include "ui/animation.h"
#include "ui/native_menu.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include <string.h>
#endif
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif

#define DIALOG_MARGIN 10   /* default content inset inside the box */
#define DIALOG_SHADOW  4   /* default solid drop-shadow offset */

/* ---------------------------------------------------------------------- *
 * Style                                                                  *
 * ---------------------------------------------------------------------- */

void dialog_style_default(struct dialog_style *s)
{
    s->box_fg           = DIALOG_COLOR_INHERIT;
    s->box_bg           = DIALOG_COLOR_INHERIT;
    s->box_border_color = DIALOG_COLOR_INHERIT;
    s->box_border_width  = 1;
    s->box_margin        = DIALOG_MARGIN;
    s->box_font          = DIALOG_FONT_INHERIT;
    s->edge_margin       = 12;
    s->box_shadow_color  = LCD_BLACK;
    s->box_shadow_offset = DIALOG_SHADOW;

    s->button_fg                     = DIALOG_COLOR_INHERIT;
    s->button_bg                     = DIALOG_COLOR_INHERIT;
    s->button_border_color           = DIALOG_COLOR_INHERIT;
    s->button_fg_selected            = DIALOG_COLOR_INHERIT;
    s->button_bg_selected            = DIALOG_COLOR_INHERIT;
    s->button_border_color_selected  = DIALOG_COLOR_INHERIT;
    s->button_border_width           = 1;
    s->button_border_radius          = 0;
    s->button_font                   = DIALOG_FONT_INHERIT;
    s->button_gap                    = 18;
    s->button_pad_x                  = 12;
    s->button_pad_y                  = 5;
}

static struct dialog_style default_style;
static bool default_style_valid;

void dialog_set_default_style(const struct dialog_style *s)
{
    if (s)
        default_style = *s;
    else
        dialog_style_default(&default_style);
    default_style_valid = true;
}

const struct dialog_style *dialog_get_default_style(void)
{
    if (!default_style_valid)
        dialog_set_default_style(NULL);
    return &default_style;
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

/* How far the box's fill is lifted from a dark background toward the text,
 * out of 256 (about 8%), and the contrast the text must keep on it: 7:1, or
 * 85% of the pair's own where the pair has less to give. */
#define DIALOG_LIFT          20
#define DIALOG_LIFT_FLOOR    700
#define DIALOG_LIFT_SHARE    85

/* Outlines are the fill this far toward the text, out of 256 (about 35%):
 * enough to mark the shape without competing with the selection. */
#define DIALOG_TINT          90

/* The shadow is the background this far toward black, out of 256. */
#define DIALOG_SHADE         128

/* A pair with no colour in it -- white on black -- dims the selected button
 * this far toward the fill, out of 256, so it is not a block of pure white:
 * white on black gives #d0d0d0. */
#define DIALOG_GREY_DIM      47
#define DIALOG_GREY_CHROMA   1311   /* 0.02 in color_chroma()'s units */

/* An unusable theme pair. A dialog fills with the background, borders and
 * draws its text in the foreground, so a pair with no contrast between them
 * renders the whole box invisible rather than merely ugly.
 *
 * Tested where the pair is resolved rather than where it is drawn, so the
 * accent -- derived from both -- comes off the substitute too, and every
 * dialog inherits the guard instead of each one carrying its own.
 * Idempotent: the substitute below is not itself broken. */
static bool theme_pair_broken(unsigned fg, unsigned bg)
{
    return (fg == bg) ||
           (bg == 63422 && fg == 65535); /* iPod reFresh '22 themes */
}

/* The legible pair swapped in when the theme's is not: a gray box with a
 * black border and black text. */
#define DIALOG_FALLBACK_FG  LCD_BLACK
#define DIALOG_FALLBACK_BG  LCD_LIGHTGRAY

/* The tones, CIELAB L*, a dark theme's dialog takes the album's colours to:
 * Scrim's own `dominant<15` and `vivid>85`, so the box matches the screen
 * behind it rather than standing out as the album's raw colour. */
#define DIALOG_DARK_TONE     15
#define DIALOG_LIGHT_TONE    85

/* The theme's live foreground and background. With dynamic colours running,
 * a theme whose own background is the darker of its pair gets the album's
 * dominant colour taken dark and its vivid one taken light, whichever way
 * round the album's pair fell. Otherwise the theme's pair as resolved, which
 * dynamic_colors_resolve() maps by value to the album's.
 *
 * The toned pair is kept until the album's colours change: every colour of
 * every button comes through here, and each tone bound is a bisection over
 * OKLab costing thousands of cycles. Nothing here yields, so the cache needs
 * no lock. */
static void dialog_theme_pair(unsigned *fg, unsigned *bg)
{
    static unsigned toned_dominant, toned_vivid, toned_fg, toned_bg;
    static bool toned_valid;
    unsigned black = LCD_RGBPACK(0, 0, 0);
    unsigned dominant, vivid;

    if (color_contrast(global_settings.bg_color, black) <
            color_contrast(global_settings.fg_color, black) &&
        dynamic_colors_roles(&dominant, &vivid))
    {
        if (!toned_valid || dominant != toned_dominant ||
            vivid != toned_vivid)
        {
            toned_fg = color_tone_bound(vivid, DIALOG_LIGHT_TONE, 1);
            toned_bg = color_tone_bound(dominant, DIALOG_DARK_TONE, -1);
            toned_dominant = dominant;
            toned_vivid = vivid;
            toned_valid = true;
        }
        *fg = toned_fg;
        *bg = toned_bg;
        return;
    }

    *fg = dynamic_colors_resolve(global_settings.fg_color);
    *bg = dynamic_colors_resolve(global_settings.bg_color);
    if (theme_pair_broken(*fg, *bg))
    {
        *fg = DIALOG_FALLBACK_FG;
        *bg = DIALOG_FALLBACK_BG;
    }
}

static unsigned dialog_theme_fg(void)
{
    unsigned fg, bg;

    dialog_theme_pair(&fg, &bg);
    return fg;
}

static unsigned dialog_theme_bg(void)
{
    unsigned fg, bg;

    dialog_theme_pair(&fg, &bg);
    return bg;
}

/* The box's fill. A dark background is lifted toward the text so the box
 * reads as a surface over the screen, where no shadow can show; a light one
 * is left alone, and the shadow does that job. */
static unsigned dialog_surface(void)
{
    unsigned fg, bg;
    unsigned black = LCD_RGBPACK(0, 0, 0);
    int floor;
    int t;

    dialog_theme_pair(&fg, &bg);
    floor = color_contrast(fg, bg) * DIALOG_LIFT_SHARE / 100;
    if (color_contrast(bg, black) >= color_contrast(fg, black))
        return bg;
    if (floor > DIALOG_LIFT_FLOOR)
        floor = DIALOG_LIFT_FLOOR;
    for (t = DIALOG_LIFT; t > 0; t -= 2)
    {
        unsigned s = color_blend(bg, fg, t);
        if (color_contrast(fg, s) >= floor)
            return s;
    }
    return bg;
}

/* The selected button's fill: the text colour, so the selection is the
 * album's own colour and its label, in the fill's, keeps the pair's contrast
 * less only the lift. */
static unsigned dialog_accent(void)
{
    unsigned fg, bg;

    dialog_theme_pair(&fg, &bg);
    if (color_chroma(fg) < DIALOG_GREY_CHROMA &&
        color_chroma(bg) < DIALOG_GREY_CHROMA)
        return color_blend(fg, dialog_surface(), DIALOG_GREY_DIM);
    return fg;
}

/* The absolute sentinels. Returns false for anything else, including INHERIT,
 * which is screen-relative and resolved by the callers below. */
static bool resolve_derived(unsigned color, unsigned *out)
{
    switch (color)
    {
        case DIALOG_COLOR_FG:
            *out = dialog_theme_fg();
            return true;
        case DIALOG_COLOR_BG:
            *out = dialog_theme_bg();
            return true;
        case DIALOG_COLOR_SURFACE:
        case DIALOG_COLOR_ON_ACCENT:
            *out = dialog_surface();
            return true;
        case DIALOG_COLOR_TINT:
            *out = color_blend(dialog_surface(), dialog_theme_fg(),
                               DIALOG_TINT);
            return true;
        case DIALOG_COLOR_SHADE:
            *out = color_blend(dialog_theme_bg(), LCD_BLACK, DIALOG_SHADE);
            return true;
        case DIALOG_COLOR_ACCENT:
            *out = dialog_accent();
            return true;
    }
    return false;
}

/* INHERIT takes the screen's own pair, which is the theme's whenever the box
 * has not already been framed -- so it needs the same guard as the derived
 * colours above. Inside a framed box the screen carries the resolved pair,
 * which the test then passes through unchanged. */
static unsigned resolve_fg(struct screen *s, unsigned color)
{
    unsigned derived;

    if (resolve_derived(color, &derived))
        return derived;
    if (color != DIALOG_COLOR_INHERIT)
        return color;
    return theme_pair_broken(s->get_foreground(), s->get_background())
           ? DIALOG_FALLBACK_FG : s->get_foreground();
}

static unsigned resolve_bg(struct screen *s, unsigned color)
{
    unsigned derived;

    if (resolve_derived(color, &derived))
        return derived;
    if (color != DIALOG_COLOR_INHERIT)
        return color;
    return theme_pair_broken(s->get_foreground(), s->get_background())
           ? DIALOG_FALLBACK_BG : s->get_background();
}

/* Flushes the part of the drop shadow that falls outside `box`: the strip down
 * its right edge and the one along its bottom. The caller flushes `box` and
 * nothing else, so without this the shadow would sit in the framebuffer and
 * never reach the display. The part *under* the box is deliberately not sent --
 * it is about to be overdrawn, and flushing it here would put a half-drawn box
 * on screen for a frame.
 *
 * lcd_update_rect() does not clip (it walks the framebuffer straight from
 * FBADDR(x, y)), so a box against the right or bottom edge would run off the
 * end. The strips are clamped here rather than trusting the driver.
 *
 * Call this only once the box itself has been drawn -- the same routine rounds
 * x down and width up to even, so a strip can overlap the box by a column, and
 * sending that column before it is drawn is what makes the edge flicker. */
static void update_shadow_strips(struct screen *screen,
                                 const struct viewport *box, int off)
{
    int sx = box->x + off;               /* shadow rect origin */
    int sy = box->y + off;
    int rx = box->x + box->width;        /* right strip starts here */
    int by = box->y + box->height;       /* bottom strip starts here */
    int x2 = sx + box->width;            /* shadow rect far edge */
    int y2 = sy + box->height;

    if (x2 > screen->lcdwidth)
        x2 = screen->lcdwidth;
    if (y2 > screen->lcdheight)
        y2 = screen->lcdheight;

    if (x2 > rx && y2 > sy)
        screen->update_rect(rx, sy, x2 - rx, y2 - sy);
    if (y2 > by && x2 > sx)
        screen->update_rect(sx, by, x2 - sx, y2 - by);
}

void dialog_frame_box(struct screen *screen, struct viewport *box,
                      const struct dialog_style *style,
                      struct viewport *content_out)
{
    /* box must already be the active viewport: set_viewport() reloads fg/bg
     * from the viewport's stored patterns on colour targets, which would undo
     * a caller's colour override (e.g. the popup's broken-theme fallback, which
     * is what DIALOG_COLOR_INHERIT picks up here). */
    int bw = style->box_border_width;
    struct dialog_insets in;

    /* Fill an opaque box, then the border. On colour targets fill with a SOLID
     * background colour via DRMODE_FG -- not DRMODE_INVERSEVID, which draws the
     * "background" and, when the theme has a backdrop active, copies the
     * backdrop image into the box (leaving it looking unfilled with unreadable
     * text over it). */
    if (screen->depth > 1)
    {
        unsigned fg = resolve_fg(screen, style->box_fg);
        unsigned bg = resolve_bg(screen, style->box_bg);
        unsigned bc = resolve_fg(screen, style->box_border_color);
        unsigned sc = resolve_fg(screen, style->box_shadow_color);

        /* The shadow falls outside `box`, so it needs a viewport that reaches
         * the rest of the screen. Every colour above is resolved BEFORE that
         * switch: resolve_*() read the screen's current fg/bg, and for the
         * popup those screen colours are the broken-theme override that
         * DIALOG_COLOR_INHERIT is there to pick up -- set_viewport() reloads
         * them from the viewport's own patterns and would lose it. */
        if (style->box_shadow_offset > 0)
        {
            struct viewport full, *prev;
            int off = style->box_shadow_offset;

            /* Both fields before the call, not after: viewport_set_fullscreen()
             * hands straight to lcd_init_viewport(), which reads
             * vp->buffer->elems -- on an uninitialised stack viewport that is a
             * garbage pointer and a data abort. Same order as
             * viewport_set_defaults(). */
            full.buffer = NULL;               /* use the default framebuffer */
            full.flags  = VP_DEFAULT_FLAGS;
            viewport_set_fullscreen(&full, screen->screen_type);
            prev = screen->set_viewport(&full);
            screen->set_drawmode(DRMODE_FG);
            screen->set_foreground(sc);
            screen->fillrect(box->x + off, box->y + off,
                             box->width, box->height);
            screen->set_drawmode(DRMODE_SOLID);
            screen->set_viewport(prev);
        }

        screen->set_drawmode(DRMODE_FG);
        screen->set_foreground(bg);          /* fill with the background colour */
        screen->fillrect(0, 0, box->width, box->height);
        screen->set_foreground(bc);
        draw_box_border(screen, box->width, box->height, bw);
        screen->set_foreground(fg);          /* interior draws in the box fg */
        screen->set_drawmode(DRMODE_SOLID);

        /* Flushed here, after the box, and not next to the shadow fill above:
         * lcd_update_rect() rounds x down and width up to even, so the right
         * strip reaches up to a column into the box. Sent before the box was
         * drawn, that column carried whatever the status-bar skin had left in
         * the framebuffer during get_action, and the box's own flush then put
         * the border back -- two different values pushed every pass, which
         * reads as a flickering right edge. */
        if (style->box_shadow_offset > 0)
            update_shadow_strips(screen, box, style->box_shadow_offset);

        /* so set_viewport(content) reloads the style's colours, not the theme's */
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
    d->cb = cb;
    d->data = data;
    d->context = context;
    d->title = title;
    d->style = style ? *style : *dialog_get_default_style();
    d->backlight_on = true;
    d->fb = NULL;
    d->present = NULL;
    d->keymap = NULL;
    d->slide = DIALOG_SLIDE_NONE;
}

/* Frame + draw the dialog on one screen. The theme parent viewport supplies the
 * font and (unless the style overrides them) the colours; the interior is the
 * callback's job. Only the box region is flushed, so the rest of the screen is
 * left untouched. */
static void dialog_draw_one(struct dialog *d, struct screen *s, int screen_nr,
                            struct viewport *bounds)
{
    struct viewport parent = d->parent[screen_nr];  /* theme font/colours */
    struct viewport box, content;
    struct viewport *outer;

    if (d->style.box_font != DIALOG_FONT_INHERIT)
        parent.font = d->style.box_font;
    if (d->fb)
    {
        /* the shadow is drawn on the screen, outside the box's buffer */
        d->style.box_shadow_offset = 0;
        parent.buffer = d->fb;
        parent.x = parent.y = 0;
        parent.width = d->fb_w;
        parent.height = d->fb_h;
    }

    /* measure() runs with the parent active, so its text metrics see the font
     * the box will actually be drawn in */
    outer = s->set_viewport(&parent);

    box = parent;
    if (!d->cb->measure)
    {
        viewport_set_fullscreen(&box, screen_nr);
        s->set_viewport(&box);
        d->cb->draw(d, s, &box, d->data);
        s->update_viewport();
        s->set_viewport(outer);
        return;
    }
    d->cb->measure(d, s, &box, d->data);            /* final geometry */
    if (bounds)
        *bounds = box;
    s->set_viewport_ex(&box, VP_FLAG_VP_SET_CLEAN);
    dialog_frame_box(s, &box, &d->style, &content);

    s->set_viewport_ex(&content, VP_FLAG_VP_SET_CLEAN);
    d->cb->draw(d, s, &content, d->data);

    s->set_viewport(&box);
    if (!d->fb)
        s->update_viewport();
    s->set_viewport(outer);
    if (d->fb)
        d->present();
}

#ifdef DIALOG_SLIDES
#define DIALOG_OPEN_FRAMES ANIM_F(12)
#define DIALOG_CLOSE_FRAMES ANIM_F(10)
#define DIALOG_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define NATIVE_DIALOG_GLYPHS 64

struct dialog_native_ui;

struct dialog_motion {
    struct ui_tween tween; /* the box's top edge */
    struct viewport box;
    unsigned int thread;
    int handle;
    int shown;
    bool entering;
    struct dialog_native_ui *native;
};

static struct dialog_motion *motion_owner;

enum native_tile_id {
    NATIVE_SHADOW, NATIVE_FRAME, NATIVE_MESSAGE, NATIVE_BUTTON_0,
    NATIVE_BUTTON_1, NATIVE_TILE_COUNT
};

struct dialog_native_ui {
    struct dialog *dialog;
    struct ui_theme theme;
    struct ui_nav nav;
    struct ui_page base_page, overlay_page;
    struct ui_view base, root, views[NATIVE_TILE_COUNT];
    struct ui_layer backdrop, layers[NATIVE_TILE_COUNT];
    int disposition, body_font;
    bool attached;
};

static bool dialog_native_action(struct ui_view *view, int action)
{
    struct dialog_native_ui *ui = view->data;
    struct dialog *d = ui->dialog;
    ui->disposition = d->cb->on_action(d, action, d->data);
    return true;
}

static void dialog_native_init(struct dialog_native_ui *ui, struct dialog *d,
                                int handle)
{
    static const struct ui_view_ops action_ops = {.action = dialog_native_action};
    static const struct ui_view_ops tile_ops = {.paint = ui_layer_paint};
    const struct ui_rect screen = {0, 0, LCD_WIDTH, LCD_HEIGHT};

    ui->dialog = d;
    ui_theme_snapshot(&ui->theme);
    ui->body_font = ui_theme_font_load(&ui->theme, UI_FONT_BODY, NATIVE_DIALOG_GLYPHS);
    ui->backdrop = (struct ui_layer){
        .handle = handle, .width = LCD_WIDTH, .height = LCD_HEIGHT,
        .offset_pixels = d->slide == DIALOG_SLIDE_EVENT ? DIALOG_PIXELS : 0,
    };
    ui_view_init(&ui->base, screen, &tile_ops, &ui->backdrop);
    ui_view_init(&ui->root, screen, &action_ops, ui);
    ui_page_init(&ui->base_page, &ui->base);
    ui_page_init(&ui->overlay_page, &ui->root);
    for (int i = 0; i < NATIVE_TILE_COUNT; ++i)
    {
        ui_view_init(&ui->views[i], (struct ui_rect){0}, &tile_ops, &ui->layers[i]);
        ui_view_append(&ui->root, &ui->views[i]);
    }
    ui_nav_push(&ui->nav, &ui->base_page, UI_PAGE);
    ui_nav_push(&ui->nav, &ui->overlay_page, UI_POPUP);
    ui->attached = true;
}

static void dialog_native_detach(struct dialog_native_ui *ui)
{
    ui_nav_pop(&ui->nav);
    ui_nav_pop(&ui->nav);
    ui->attached = false;
    if (ui->body_font >= 0)
        font_unload(ui->body_font);
}

static uint32_t dialog_rgb888(unsigned rgb565)
{
    return (RGB_UNPACK_RED(rgb565) << 16) | (RGB_UNPACK_GREEN(rgb565) << 8) |
            RGB_UNPACK_BLUE(rgb565);
}

static unsigned dialog_native_color(const struct ui_theme *theme,
                                     enum ui_color_role role,
                                     const struct ui_album_palette *palette)
{
    uint32_t color = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(color >> 16, (color >> 8) & 255, color & 255);
}

static struct dialog_style dialog_native_style(const struct dialog_native_ui *ui)
{
    struct ui_album_palette palette = {0};
    unsigned dominant, vivid;
    if (dynamic_colors_roles(&dominant, &vivid))
    {
        palette.has_dominant = palette.has_vivid = true;
        palette.dominant = dialog_rgb888(dominant);
        palette.vivid = dialog_rgb888(vivid);
    }
    const struct ui_theme *theme = &ui->theme;
    const uint16_t *metric = theme->metric;
    unsigned bg = dialog_native_color(theme, UI_COLOR_MODAL_BG, &palette);
    unsigned fg = dialog_native_color(theme, UI_COLOR_MODAL_FG, &palette);
    unsigned border = dialog_native_color(theme, UI_COLOR_MODAL_BORDER, &palette);
    unsigned accent = dialog_native_color(theme, UI_COLOR_MODAL_ACCENT, &palette);
    unsigned selected_text = dialog_native_color(theme, UI_COLOR_MODAL_SELECTED_TEXT,
                                                  &palette);
    return (struct dialog_style){
        .box_fg = fg, .box_bg = bg, .box_border_color = border,
        .box_border_width = metric[UI_METRIC_MODAL_BORDER_WIDTH],
        .box_margin = metric[UI_METRIC_MODAL_PADDING],
        .box_font = ui->body_font >= 0 ? ui->body_font : DIALOG_FONT_INHERIT,
        .edge_margin = metric[UI_METRIC_MODAL_EDGE_MARGIN],
        .box_shadow_color = dialog_native_color(theme, UI_COLOR_MODAL_SHADOW, &palette),
        .box_shadow_offset = metric[UI_METRIC_MODAL_SHADOW_OFFSET],
        .button_fg = fg, .button_bg = bg, .button_border_color = border,
        .button_fg_selected = selected_text,
        .button_bg_selected = accent, .button_border_color_selected = accent,
        .button_border_width = metric[UI_METRIC_MODAL_BORDER_WIDTH],
        .button_font = DIALOG_FONT_INHERIT,
        .button_gap = metric[UI_METRIC_MODAL_BUTTON_GAP],
        .button_pad_x = metric[UI_METRIC_MODAL_BUTTON_PAD_X],
        .button_pad_y = metric[UI_METRIC_MODAL_BUTTON_PAD_Y],
    };
}

static struct ui_rect dialog_rect(const struct viewport *vp)
{
    return (struct ui_rect){vp->x, vp->y, vp->width, vp->height};
}

static void dialog_native_tile(struct dialog_native_ui *ui, int tile,
                                struct ui_rect bounds, size_t offset,
                                bool solid)
{
    struct ui_page *p = &ui->overlay_page;
    struct ui_view *v = &ui->views[tile];
    struct ui_layer *layer = &ui->layers[tile];
    ui_page_set(p, v, UI_X, bounds.x);
    ui_page_set(p, v, UI_Y, bounds.y);
    ui_page_set(p, v, UI_WIDTH, bounds.width);
    ui_page_set(p, v, UI_HEIGHT, bounds.height);
    ui_page_invalidate(p, bounds);
    *layer = (struct ui_layer){
        .handle = ui->backdrop.handle, .offset_pixels = offset,
        .width = solid ? 1 : bounds.width,
        .height = solid ? 1 : bounds.height,
    };
}

/* Draw into a packed opaque texture. All high-level modal paints pass through
 * the retained tree; the firmware's glyph and rectangle routines are shared
 * raster primitives. The compositor clips, scales and blends these textures. */
static struct viewport dialog_tile_viewport(struct viewport style,
                                             struct frame_buffer_t *buffer,
                                             fb_data *pixels, int w, int h)
{
    *buffer = (struct frame_buffer_t){.fb_ptr = pixels, .stride = w, .elems = w * h};
    style.buffer = buffer;
    style.x = style.y = 0;
    style.width = w;
    style.height = h;
    style.drawmode = DRMODE_FG;
    return style;
}

static bool dialog_native_draw(struct dialog_native_ui *ui)
{
    struct dialog *d = ui->dialog;
    struct screen *s = &screens[SCREEN_MAIN];
    struct viewport box = d->parent[SCREEN_MAIN];
    if (2 * (d->style.edge_margin + d->style.box_margin) >= LCD_WIDTH ||
        2 * d->style.box_margin >= LCD_HEIGHT)
        return false;
    if (d->style.box_font != DIALOG_FONT_INHERIT)
        box.font = d->style.box_font;
    struct viewport *prev = s->set_viewport(&box);
    d->cb->measure(d, s, &box, d->data);
    struct dialog_insets in;
    dialog_get_insets(&d->style, &in);
    struct viewport content = box;
    content.x += in.left;
    content.y += in.top;
    content.width -= in.left + in.right;
    content.height -= in.top + in.bottom;
    if (box.x < 0 || box.y < 0 || box.width <= 0 || box.height <= 0 ||
        box.width > LCD_WIDTH - box.x || box.height > LCD_HEIGHT - box.y ||
        content.width <= 0 || content.height <= 0)
    {
        s->set_viewport(prev);
        return false;
    }

    struct dialog_native_content model = {0};
    int line_height = s->getcharheight();
    s->set_viewport(&content);
    d->cb->native_layout(d, s, &content, &model, d->data);
    if (model.line_count < 0 || model.line_count > DIALOG_NATIVE_LINES ||
        model.button_count < 0 || model.button_count > DIALOG_NATIVE_BUTTONS)
    {
        s->set_viewport(prev);
        return false;
    }
    int message_h = line_height * model.line_count;
    size_t pixels = 1 + box.width * box.height + content.width * message_h;
    bool valid = message_h <= content.height;
    for (int i = 0; i < model.button_count; ++i)
    {
        const struct dialog_native_button *b = &model.buttons[i];
        valid &= b->x >= 0 && b->y >= 0 && b->width > 0 && b->height > 0 &&
                 b->width <= content.width - b->x && b->height <= content.height - b->y;
        pixels += b->width * b->height;
    }
    if (!valid || pixels > DIALOG_PIXELS)
    {
        s->set_viewport(prev);
        return false;
    }

    size_t offset = ui->backdrop.offset_pixels + DIALOG_PIXELS;
    fb_data *base = core_get_data_pinned(ui->backdrop.handle);
    struct ui_rect shadow = dialog_rect(&box);
    if (d->style.box_shadow_offset > 0)
    {
        shadow.x += d->style.box_shadow_offset;
        shadow.y += d->style.box_shadow_offset;
        base[offset] = resolve_fg(s, d->style.box_shadow_color);
    }
    else
        shadow.width = 0;
    dialog_native_tile(ui, NATIVE_SHADOW, shadow, offset++, true);

    struct frame_buffer_t buffer;
    struct viewport tile = dialog_tile_viewport(box, &buffer, base + offset,
                                                 box.width, box.height);
    struct viewport framed_content;
    struct dialog_style frame_style = d->style;
    frame_style.box_shadow_offset = 0; /* shadow is a separate retained node */
    s->set_viewport(&tile);
    dialog_frame_box(s, &tile, &frame_style, &framed_content);
    content.fg_pattern = framed_content.fg_pattern;
    content.bg_pattern = framed_content.bg_pattern;
    dialog_native_tile(ui, NATIVE_FRAME, dialog_rect(&box), offset, false);
    offset += box.width * box.height;

    struct ui_rect message = {content.x, content.y, content.width, message_h};
    if (message_h > 0)
    {
        tile = dialog_tile_viewport(content, &buffer, base + offset,
                                     message.width, message.height);
        tile.flags |= VP_FLAG_ALIGN_CENTER;
        s->set_viewport(&tile);
        s->set_foreground(content.bg_pattern);
        s->fillrect(0, 0, tile.width, tile.height);
        s->set_foreground(content.fg_pattern);
        for (int i = 0; i < model.line_count; ++i)
            s->putsxyf(0, i * line_height, "%.*s",
                        model.lines[i].len, model.lines[i].str);
    }
    dialog_native_tile(ui, NATIVE_MESSAGE, message, offset, false);
    offset += message.width * message.height;

    for (int i = 0; i < DIALOG_NATIVE_BUTTONS; ++i)
    {
        struct ui_rect bounds = {0};
        if (i < model.button_count)
        {
            const struct dialog_native_button *b = &model.buttons[i];
            bounds = (struct ui_rect){content.x + b->x, content.y + b->y,
                                      b->width, b->height};
            tile = dialog_tile_viewport(content, &buffer, base + offset,
                                         b->width, b->height);
            s->set_viewport(&tile);
            s->set_foreground(content.bg_pattern);
            s->fillrect(0, 0, tile.width, tile.height);
            s->set_foreground(content.fg_pattern);
            dialog_draw_button(s, &d->style, 0, 0, b->width, b->height,
                               b->label, b->selected);
        }
        dialog_native_tile(ui, NATIVE_BUTTON_0 + i, bounds, offset, false);
        offset += bounds.width * bounds.height;
    }
    s->set_viewport(prev);
    core_put_data_pinned(base);
    ui_nav_draw(&ui->nav);
    return true;
}

static bool dialog_native_paint(struct dialog_native_ui *ui)
{
    struct dialog *d = ui->dialog;
    struct dialog_style legacy_style = d->style;
    d->style = dialog_native_style(ui);
    bool painted = dialog_native_draw(ui);
    d->style = legacy_style;
    return painted;
}

static bool dialog_native_supported(const struct dialog *d)
{
    if (d->slide == DIALOG_SLIDE_NONE || !d->cb->native_layout || !ui_native_enabled())
        return false;

    /* Scrim's album-derived dialog mixes legacy surface and accent formulas.
     * Defer that variant until it has parity evidence on album states. */
    unsigned dominant, vivid;
    return strcmp(global_settings.native_theme, "scrim") ||
           !dynamic_colors_roles(&dominant, &vivid);
}

/* The active viewport can belong to the invoking list or skin. Always
 * restore the default framebuffer and present it through the default view. */
static void dialog_restore_display(void)
{
    struct viewport *prev = lcd_set_viewport(NULL);
    lcd_update();
    lcd_set_viewport(prev);
}

static void dialog_release_transition(bool restore)
{
    struct dialog_motion *m = motion_owner;
    if (!m || m->thread != thread_self())
        return;
    if (m->native)
    {
        dialog_native_detach(m->native);
        m->native = NULL;
    }
    if (restore)
    {
        fb_data *base = core_get_data_pinned(m->handle);
        memcpy(lcd_framebuffer_default.fb_ptr, base, FRAMEBUFFER_SIZE);
        core_put_data_pinned(base);
    }
    motion_owner = NULL;
    m->handle = core_free(m->handle);
    if (restore && lcd_active())
        dialog_restore_display();
}

void dialog_interrupt_transition(void)
{
    dialog_release_transition(true);
}

bool dialog_prepare_usb(void)
{
    bool nested = motion_owner && motion_owner->thread == thread_self() &&
                  motion_owner->native;
    dialog_release_transition(!nested);
    return nested;
}

static void dialog_begin_transition(struct dialog_motion *m, const struct dialog *d,
                                     bool native)
{
    if ((!native && (d->slide == DIALOG_SLIDE_NONE || !global_settings.animations)) ||
        !d->cb->measure ||
        d->fb || motion_owner || !lcd_active())
        return;

    ui_scene_interrupt();
    if (lcd_updates_held())
        return;

    int surfaces = native && d->slide == DIALOG_SLIDE_EVENT ? 3 : 2;
    m->handle = core_alloc_free_space(surfaces * FRAMEBUFFER_SIZE);
    if (m->handle <= 0)
        return;

    fb_data *base = core_get_data_pinned(m->handle);
    memcpy(base, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    core_put_data_pinned(base);
    m->thread = thread_self();
    m->shown = LCD_HEIGHT;
    m->entering = !native;
    motion_owner = m;
}

/* Copy a clipped box as it moves down, leaving the page outside it intact.
 * At rest, include the original theme's shadow, if any. */
static void dialog_blit_rect(fb_data *out, const fb_data *layer,
                             int x, int y, int w, int h, int offset)
{
    int x1 = MAX(0, x);
    int x2 = MIN(LCD_WIDTH, x + w);
    int y1 = MAX(0, y);
    int y2 = MIN(LCD_HEIGHT - offset, y + h);
    if (x1 >= x2)
        return;
    for (int row = y1; row < y2; ++row)
        memcpy(out + (row + offset) * LCD_WIDTH + x1, layer + row * LCD_WIDTH + x1,
               (x2 - x1) * sizeof(*out));
}

static void dialog_paint_layer(struct dialog_motion *m, struct dialog *d)
{
    /* The normal painter resolves all text, theme colours and geometry. */
    if (m->entering)
        lcd_hold_updates(true);
    dialog_draw_one(d, &screens[SCREEN_MAIN], SCREEN_MAIN, &m->box);
    if (m->entering)
        lcd_hold_updates(false);

    fb_data *surfaces = core_get_data_pinned(m->handle);
    const struct viewport *box = &m->box;
    int shadow = MAX(0, d->style.box_shadow_offset);
    dialog_blit_rect(surfaces + DIALOG_PIXELS, lcd_framebuffer_default.fb_ptr,
                     box->x, box->y, box->width + shadow, box->height + shadow, 0);
    core_put_data_pinned(surfaces);
}

static bool dialog_show_transition(struct dialog_motion *m, int y, bool final)
{
    fb_data *out = lcd_framebuffer_default.fb_ptr;
    fb_data *surfaces = core_get_data_pinned(m->handle);
    const fb_data *layer = surfaces + DIALOG_PIXELS;
    const struct viewport *box = &m->box;
    if (final)
        memcpy(out, layer, FRAMEBUFFER_SIZE);
    else
    {
        memcpy(out, surfaces, FRAMEBUFFER_SIZE);
        dialog_blit_rect(out, layer, box->x, box->y, box->width, box->height,
                         MAX(0, y - box->y));
    }
    bool presented = lcd_present_scene(out);
    if (presented)
        m->shown = y;
    core_put_data_pinned(surfaces);
    return presented;
}

static void dialog_close_transition(struct dialog_motion *m)
{
    /* If accepted during entry, continue from the position on screen. */
    m->tween = (struct ui_tween){
        .start_clock = USEC_TIMER, .from = m->shown, .to = LCD_HEIGHT,
        .frames = DIALOG_CLOSE_FRAMES, .ease_in = true,
    };
    while (lcd_active() && m->shown < LCD_HEIGHT)
    {
        unsigned elapsed = (uint32_t)USEC_TIMER - m->tween.start_clock;
        int ticks = ui_clock_timeout(elapsed, m->tween.frames, HZ);
        if (!ticks || button_queue_wait_pending(ticks))
            break;
        elapsed = (uint32_t)USEC_TIMER - m->tween.start_clock;
        unsigned frame = ui_clock_frame(elapsed);
        int y = ui_tween_sample(&m->tween, frame);
        if (y == LCD_HEIGHT)
            break;
        if (y != m->shown)
            dialog_show_transition(m, y, false);
    }
    dialog_interrupt_transition();
}
#else
void dialog_interrupt_transition(void)
{
}
bool dialog_prepare_usb(void)
{
    return false;
}
#endif

/* The legacy path does not reserve a retained tree on its stack. */
struct dialog_native_ui;
static int dialog_run_loop(struct dialog *d, int poll_ticks,
                            struct dialog_native_ui *native)
{
    int disp = DIALOG_CONTINUE;

#ifdef DIALOG_SLIDES
    struct dialog_motion motion = {0};
    dialog_begin_transition(&motion, d, native != NULL);
    bool repaint = true;
    bool animate_close = false;
#else
    (void)native;
#endif
    d->poll_ticks = poll_ticks;

    skin_hold_scrolling(true);
    FOR_NB_SCREENS(i)
    {
        screens[i].scroll_stop();
        sb_set_persistent_title(d->title, Icon_NOICON, i);
        viewportmanager_theme_enable(i, true, &d->parent[i]);
    }

#ifdef DIALOG_SLIDES
    if (motion.handle > 0)
    {
        fb_data *surfaces = core_get_data_pinned(motion.handle);
        /* The action dispatcher shows the consent's chrome. A direct
         * confirmation presents only its box over the invoking page. */
        const fb_data *settled = d->slide == DIALOG_SLIDE_EVENT ?
            lcd_framebuffer_default.fb_ptr : surfaces;
        memcpy(surfaces + DIALOG_PIXELS, settled, FRAMEBUFFER_SIZE);
        /* Enabling the old theme for consent can clear the retained menu
         * body. Keep its live rows beneath the event's temporary header. */
        int status_height = d->slide == DIALOG_SLIDE_EVENT ?
                            native_menu_status_height() : 0;
        if (status_height > 0)
        {
            size_t header_pixels = status_height * LCD_WIDTH;
            memcpy(surfaces + DIALOG_PIXELS + header_pixels,
                   surfaces + header_pixels,
                   FRAMEBUFFER_SIZE - header_pixels * sizeof(fb_data));
        }
        core_put_data_pinned(surfaces);
        if (native)
        {
            dialog_native_init(native, d, motion.handle);
            motion.native = native;
        }
    }
#endif

    /* eat any stray keypresses from before the dialog opened */
#ifdef DIALOG_SLIDES
    if (motion.handle > 0)
        action_wait_for_release_preserve_queue();
    else
#endif
        action_wait_for_release();

    while (disp == DIALOG_CONTINUE)
    {
        /* Repaint every pass. The status-bar skin renders into the framebuffer
         * on GUI_EVENT_ACTIONUPDATE (fired inside get_action) and dirties the
         * box region -- its flush is inhibited, but we still have to repaint
         * over it, the same way the list and album-covers screens do. */
        FOR_NB_SCREENS(i)
        {
#ifdef DIALOG_SLIDES
            if (i == SCREEN_MAIN && native && native->attached)
            {
                if (motion.handle > 0 && dialog_native_paint(native))
                    continue;
                /* Unsupported geometry or a higher-priority screen may end
                 * the retained overlay while the confirmation stays open. */
                dialog_interrupt_transition();
            }
            if (i == SCREEN_MAIN && motion.handle > 0)
            {
                bool first = motion.tween.frames == 0;
                if (first || repaint || !motion.entering)
                    dialog_paint_layer(&motion, d);
                if (first)
                    motion.tween = (struct ui_tween){
                        .start_clock = USEC_TIMER, .from = LCD_HEIGHT,
                        .to = motion.box.y, .frames = DIALOG_OPEN_FRAMES,
                    };
                if (motion.entering)
                {
                    unsigned frame = ui_clock_frame((uint32_t)USEC_TIMER -
                                                    motion.tween.start_clock);
                    int y = ui_tween_sample(&motion.tween, frame);
                    bool final = frame >= motion.tween.frames;
                    if (y != motion.shown || repaint || final)
                    {
                        bool sent = dialog_show_transition(&motion, y, final);
                        if (final && sent)
                            motion.entering = false;
                    }
                }
                else
                    motion.shown = motion.box.y;
                continue;
            }
#endif
            dialog_draw_one(d, &screens[i], i, NULL);
        }

        /* sampled before get_action so a callback can ignore the keypress
         * that merely woke the screen (matches the old yes/no behaviour) */
        d->backlight_on = is_backlight_on(false);

        /* A dialog left open across a track change would otherwise hold the
         * previous album's colours: the loop only comes round on input, and a
         * caller waiting on TIMEOUT_BLOCK never does. Poll for as long as the
         * palette is asking to be repainted, then go back to whatever the
         * caller asked for. */
        int ticks = d->poll_ticks;
        if (dynamic_colors_needs_repaint()
            && (ticks == TIMEOUT_BLOCK || ticks > HZ / 10))
            ticks = HZ / 10;
#ifdef DIALOG_SLIDES
        if (motion.handle > 0 && motion.entering)
        {
            int wait = ui_clock_timeout((uint32_t)USEC_TIMER - motion.tween.start_clock,
                                        motion.tween.frames, HZ);
            if (!wait)
                wait = 1; /* if DMA was busy, retry the settled frame */
            if (ticks == TIMEOUT_BLOCK || ticks > wait)
                ticks = wait;
        }
#endif

        skin_inhibit_flush(true);
        int action = d->keymap ? get_custom_action(CONTEXT_PLUGIN, ticks,
                                                   d->keymap)
                               : get_action(d->context, ticks);
        skin_inhibit_flush(false);

#ifdef HAVE_WHEEL_WPS_CHORD
        /* Editors use KBD_ABORT for local focus changes and confirmations.
         * A pending WPS jump leaves the dialog through its cancel cleanup. */
        if (d->context == CONTEXT_KEYBOARD && agents_nav_pending() == AGENTS_NAV_WPS)
            disp = DIALOG_CANCEL;
        else
#endif
        {
#ifdef DIALOG_SLIDES
            if (native && native->attached && motion.handle > 0 &&
                ui_nav_dispatch(&native->nav, action))
                disp = native->disposition;
            else
#endif
                disp = d->cb->on_action(d, action, d->data);
        }
#ifdef DIALOG_SLIDES
        repaint = action != ACTION_NONE || dynamic_colors_needs_repaint();
        animate_close = disp != DIALOG_ABORT && action != ACTION_NONE;
#endif
    }

#ifdef DIALOG_SLIDES
    if (native && native->attached)
        dialog_interrupt_transition();
    if (motion.handle > 0)
    {
        if (animate_close)
            dialog_close_transition(&motion);
        else
            dialog_interrupt_transition();
    }
#endif

    if (d->cb->on_close)
        d->cb->on_close(d, d->data);

    FOR_NB_SCREENS(i)
    {
        screens[i].scroll_stop_viewport(&d->parent[i]);
        sb_set_persistent_title(d->title, Icon_NOICON, i);
        viewportmanager_theme_undo(i, false);
    }

    /* A held line was drawn static and only redraws when its text changes,
     * so ask for every line again to hand them back to the scroller -- once
     * no dialog this one sat on is still holding them. */
    if (skin_hold_scrolling(false))
    {
        skin_request_full_update(CUSTOM_STATUSBAR);
        sb_skin_force_next_update();
    }

    return disp;
}

#ifdef DIALOG_SLIDES
static int __attribute__((noinline)) dialog_run_retained(struct dialog *d,
                                                         int poll_ticks)
{
    struct dialog_native_ui native = {0};
    return dialog_run_loop(d, poll_ticks, &native);
}
#endif

int dialog_run(struct dialog *d, int poll_ticks)
{
#ifdef DIALOG_SLIDES
    if (dialog_native_supported(d))
        return dialog_run_retained(d, poll_ticks);
#endif
    return dialog_run_loop(d, poll_ticks, NULL);
}
