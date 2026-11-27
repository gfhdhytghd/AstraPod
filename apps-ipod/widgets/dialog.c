/***************************************************************************
 * Modal box primitive shared by the popup, yes/no and text-input dialogs.
 * Draws a centred bordered box and returns its content viewport; owns the
 * dialog style defaults.
 ****************************************************************************/

#include "config.h"
#include "audio.h"
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
#include "draw/gloss.h"
#include "draw/round_rect.h"
#include "draw/text_wrap.h"
#include "metadata/album_palette.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "dialog.h"
#include "core_alloc.h"
#include "system.h"
#include "thread.h"
#include "ui/frame.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/sheet.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include <string.h>
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif

#define DIALOG_GLASS_RADIUS 10
#define DIALOG_POPUP_RADIUS 10
#define DIALOG_SHADOW_PAD_X 10
#define DIALOG_SHADOW_PAD_TOP 8
#define DIALOG_SHADOW_PAD_BOT 12

static const struct gloss_fill classic_glass_trans = {
    0xdc585d64, 0xe022262c, 0xea090b0e, 0xee040507
};
static const struct gloss_fill classic_glass_opaque = {
    0xff585d64, 0xff22262c, 0xff090b0e, 0xff040507
};
static const struct gloss_fill classic_gloss_band = {
    0x40ffffff, 0x2affffff, 0x0affffff, 0x16ffffff
};
/* 2009 iPhone OS 3 UIAlertView-inspired translucent midnight-navy glass. */
static const struct gloss_fill classic_alert_body_trans = {
    0xec284578, 0xea1a325c, 0xf00c1b3a, 0xf4071228
};
static const struct gloss_fill classic_alert_body_opaque = {
    0xff284578, 0xff1a325c, 0xff0c1b3a, 0xff071228
};
/* Two-half split-gloss 2009 capsule fills: darker navy (Cancel / left) vs
 * brighter steel-cerulean (Default / right). */
static const struct gloss_fill classic_cap_sec_top = {
    0x66ffffff, 0xdc364e76, 0xe01e3456, 0xe21c3050
};
static const struct gloss_fill classic_cap_sec_bot = {
    0xe611223e, 0xe611223e, 0xe81a3054, 0xea1f365c
};
static const struct gloss_fill classic_cap_pri_top = {
    0x8affffff, 0xe2648cc6, 0xe438609c, 0xe6325892
};
static const struct gloss_fill classic_cap_pri_bot = {
    0xea204684, 0xea204684, 0xec305ea2, 0xee3868ae
};
static const struct gloss_fill classic_well_light = {
    0xff363638, 0xff46484a, 0xff767678, 0xff68686a
};
static const struct gloss_fill classic_well_dark = {
    0xf0101012, 0xf5181a1d, 0xf52e3136, 0xf03c4046
};
static const struct gloss_fill classic_pill_gloss = {
    0x44ffffff, 0x24ffffff, 0x08ffffff, 0x12ffffff
};
static const struct gloss_fill classic_track_dark = {
    0xff101216, 0xff1a1e24, 0xff2e343e, 0xff383e48
};

bool dialog_is_classic(void)
{
    return ui_theme_style(UI_STYLE_DIALOG) == UI_DIALOG_CLASSIC;
}

static unsigned theme_color(const struct ui_theme *theme, enum ui_color_role role,
                            const struct ui_album_palette *palette)
{
    uint32_t rgb = ui_theme_color(theme, role, palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static struct gloss_surface vp_gloss_surface(struct screen *screen,
                                             int *ox, int *oy)
{
    struct viewport *vp = *screen->current_viewport;
    struct frame_buffer_t *fb = vp->buffer ? vp->buffer : &lcd_framebuffer_default;
    int stride = fb->stride > 0 ? fb->stride : LCD_WIDTH;
    int fb_h = stride > 0 ? (int)(fb->elems / (size_t)stride) : LCD_HEIGHT;
    *ox = vp->x;
    *oy = vp->y;
    return (struct gloss_surface){
        .pixels = fb->fb_ptr,
        .stride = stride,
        .clip_x = 0,
        .clip_y = 0,
        .clip_w = stride,
        .clip_h = fb_h,
    };
}

static struct gloss_fill theme_selection_fill(const struct ui_theme *theme,
                                              const struct ui_album_palette *palette)
{
    const struct ui_theme_gradient *g = &theme->gradient[UI_GRADIENT_SELECTION];
    if (g->stops >= 4)
        return (struct gloss_fill){
            0xff000000u | g->color[0], 0xff000000u | g->color[1],
            0xff000000u | g->color[2], 0xff000000u | g->color[3]
        };
    if (g->stops >= 2)
        return (struct gloss_fill){
            0xff000000u | g->color[0], 0xff000000u | g->color[0],
            0xff000000u | g->color[1], 0xff000000u | g->color[1]
        };
    uint32_t rgb = ui_theme_color(theme, UI_COLOR_SELECTION, palette);
    return (struct gloss_fill){
        0xff000000u | rgb, 0xff000000u | rgb,
        0xff000000u | rgb, 0xff000000u | rgb
    };
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
    bool classic = theme.style[UI_STYLE_DIALOG] == UI_DIALOG_CLASSIC;
    *s = (struct dialog_style){
        .box_fg = classic ? LCD_RGBPACK(255, 255, 255) : fg,
        .box_bg = classic ? LCD_RGBPACK(17, 35, 70) : bg,
        .box_border_color = classic ? LCD_RGBPACK(212, 226, 246) : outline,
        .box_border_width = classic ? 1 : m[UI_METRIC_MODAL_BORDER_WIDTH],
        .box_margin = m[UI_METRIC_MODAL_PADDING],
        .box_font = DIALOG_FONT_INHERIT,
        .edge_margin = m[UI_METRIC_MODAL_EDGE_MARGIN],
        .button_fg = classic ? LCD_RGBPACK(255, 255, 255) : fg,
        .button_bg = classic ? LCD_RGBPACK(72, 74, 76) : bg,
        .button_border_color = outline,
        .button_fg_selected = classic ? LCD_RGBPACK(255, 255, 255)
            : theme_color(&theme, UI_COLOR_MODAL_SELECTED_TEXT, &palette),
        .button_bg_selected = accent,
        .button_border_color_selected = accent,
        .button_border_width = m[UI_METRIC_MODAL_BORDER_WIDTH],
        .button_border_radius = classic ? 5 : 0,
        .button_font = DIALOG_FONT_INHERIT,
        .button_gap = m[UI_METRIC_MODAL_BUTTON_GAP],
        .button_pad_x = m[UI_METRIC_MODAL_BUTTON_PAD_X],
        .button_pad_y = m[UI_METRIC_MODAL_BUTTON_PAD_Y],
        .glass = false,
        .popup = classic,
    };
}

void dialog_style_glass(struct dialog_style *s)
{
    dialog_style_default(s);
    if (!dialog_is_classic())
        return;
    s->box_fg = LCD_RGBPACK(255, 255, 255);
    s->box_bg = LCD_RGBPACK(18, 21, 26);
    s->box_border_color = LCD_RGBPACK(88, 93, 100);
    s->box_border_width = 1;
    s->box_margin = 12;
    s->button_fg = LCD_RGBPACK(255, 255, 255);
    s->button_bg = LCD_RGBPACK(30, 34, 40);
    s->button_fg_selected = LCD_RGBPACK(255, 255, 255);
    s->button_border_radius = 5;
    s->glass = true;
    s->popup = false;
}

void dialog_draw_glass_panel(struct screen *screen, int x, int y,
                             int w, int h, int r, bool opaque)
{
    int ox, oy;
    struct gloss_surface s = vp_gloss_surface(screen, &ox, &oy);
    int gx = ox + x, gy = oy + y;
    int gloss_h = MIN(36, MAX(12, h * 38 / 100));
    if (!opaque)
        gloss_shadow(&s, gx, gy, w, h, r, 6, 3, 128);
    gloss_rect(&s, gx, gy, w, h, r, r,
               opaque ? &classic_glass_opaque : &classic_glass_trans);
    if (w > 2 && h > 4)
        gloss_rect(&s, gx + 1, gy + 1, w - 2, gloss_h,
                   r > 1 ? r - 1 : 0, 0, &classic_gloss_band);
    gloss_outline(&s, gx, gy, w, h, r, 0x54ffffff);
}

static inline fb_data blend_white_565(fb_data p, unsigned a)
{
    unsigned r = (p >> 11) & 31;
    unsigned g = (p >> 5) & 63;
    unsigned b = p & 31;
    unsigned inv = 255 - a;
    r = (r * inv + 31 * a + 127) / 255;
    g = (g * inv + 63 * a + 127) / 255;
    b = (b * inv + 31 * a + 127) / 255;
    return (fb_data)((r << 11) | (g << 5) | b);
}

/* Procedural curved specular dome across the top of a 2009 glass alert:
 * a gentle parabolic arc dipping lower at centre, clipped to the inner
 * rounded top corners with 1 px vertical antialiasing along the lower arc. */
static void draw_alert_gloss_dome(const struct gloss_surface *s,
                                  int x, int y, int w, int h)
{
    if (w < 16 || h < 24 || !s->pixels || s->stride <= 0)
        return;
    int center_h = MIN(28, MAX(16, h / 4));
    int edge_h = MAX(12, center_h - 10);
    int r = DIALOG_POPUP_RADIUS - 2;
    int r2 = r * r;
    int span = w - 1;
    int span2 = span * span;
    int x_min = MAX(2, s->clip_x - x);
    int x_max = MIN(w - 3, s->clip_x + s->clip_w - 1 - x);
    int y_min = MAX(2, s->clip_y - y);
    int y_max = MIN(center_h + 1, MIN(h - 3, s->clip_y + s->clip_h - 1 - y));
    if (x_min > x_max || y_min > y_max)
        return;

    for (int dy = y_min; dy <= y_max; dy++)
    {
        fb_data *row = s->pixels + (y + dy) * s->stride + x;
        int cy = (dy < 2 + r) ? (2 + r - dy) : 0;
        int cy2 = cy * cy;
        for (int dx = x_min; dx <= x_max; dx++)
        {
            if (cy > 0)
            {
                int cx = 0;
                if (dx < 2 + r)
                    cx = 2 + r - dx;
                else if (dx > w - 3 - r)
                    cx = dx - (w - 3 - r);
                if (cx * cx + cy2 > r2)
                    continue;
            }
            int u = 2 * dx - span;
            if (u < 0)
                u = -u;
            int drop = (int)(((int64_t)(center_h - edge_h) * 256 * u * u) / span2);
            int cut_256 = (center_h << 8) - drop;
            int dy_256 = dy << 8;
            if (dy_256 >= cut_256 + 256)
                continue;
            unsigned cov = (unsigned)MIN(256, cut_256 + 256 - dy_256);
            int depth = MAX(1, (cut_256 >> 8) - 2);
            int t = MIN(depth, dy - 2);
            unsigned base_a = (unsigned)(112 - 80 * t / depth);
            unsigned a = (base_a * cov) >> 8;
            if (a > 0)
                row[dx] = blend_white_565(row[dx], a);
        }
    }
}

void dialog_draw_popup_panel(struct screen *screen, int x, int y,
                             int w, int h, bool opaque)
{
    int ox, oy;
    struct gloss_surface s = vp_gloss_surface(screen, &ox, &oy);
    int gx = ox + x, gy = oy + y;
    if (!opaque)
        gloss_shadow(&s, gx, gy, w, h, DIALOG_POPUP_RADIUS, 8, 4, 165);
    gloss_rect(&s, gx, gy, w, h, DIALOG_POPUP_RADIUS, DIALOG_POPUP_RADIUS,
               opaque ? &classic_alert_body_opaque : &classic_alert_body_trans);
    draw_alert_gloss_dome(&s, gx, gy, w, h);
    gloss_outline(&s, gx - 1, gy - 1, w + 2, h + 2,
                  DIALOG_POPUP_RADIUS + 1, 0x90030814);
    gloss_outline(&s, gx, gy, w, h, DIALOG_POPUP_RADIUS, 0xdadce8f8);
    if (w > 4 && h > 4)
        gloss_outline(&s, gx + 1, gy + 1, w - 2, h - 2,
                      DIALOG_POPUP_RADIUS - 1, 0x76c6d8f2);
}

static void draw_alert_capsule(struct screen *screen, int x, int y,
                               int w, int h, bool primary)
{
    if (w <= 6 || h <= 6)
        return;
    int ox, oy;
    struct gloss_surface s = vp_gloss_surface(screen, &ox, &oy);
    int gx = ox + x, gy = oy + y;
    const int r = 5;
    int mid = h / 2;

    /* Subtle bottom outer catchlight below the bevel. */
    gloss_outline(&s, gx, gy + 1, w, h, r, primary ? 0x38d2e4ff : 0x2cc8dcff);
    gloss_rect(&s, gx, gy, w, mid, r, 0,
               primary ? &classic_cap_pri_top : &classic_cap_sec_top);
    gloss_rect(&s, gx, gy + mid, w, h - mid, 0, r,
               primary ? &classic_cap_pri_bot : &classic_cap_sec_bot);
    gloss_outline(&s, gx, gy, w, h, r, 0xd2060f22);
}

void dialog_draw_shadow_text(struct screen *screen, int x, int y,
                             const char *text, unsigned fg, unsigned shadow,
                             int dy)
{
    unsigned prev = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(shadow);
    screen->putsxy(x, y + dy, (const unsigned char *)text);
    screen->set_foreground(fg);
    screen->putsxy(x, y, (const unsigned char *)text);
    screen->set_foreground(prev);
    screen->set_drawmode(prev_mode);
}

/* Crisp 4x7 left/right and 5x7 up/down Click Wheel hint triangles. */
static void draw_hint_tri_left(struct screen *screen, int x, int cy, unsigned fg)
{
    unsigned prev = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(fg);
    screen->drawpixel(x, cy);
    screen->vline(x + 1, cy - 1, cy + 1);
    screen->vline(x + 2, cy - 2, cy + 2);
    screen->vline(x + 3, cy - 3, cy + 3);
    screen->set_foreground(prev);
    screen->set_drawmode(prev_mode);
}

static void draw_hint_tri_right(struct screen *screen, int x, int cy, unsigned fg)
{
    unsigned prev = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(fg);
    screen->vline(x, cy - 3, cy + 3);
    screen->vline(x + 1, cy - 2, cy + 2);
    screen->vline(x + 2, cy - 1, cy + 1);
    screen->drawpixel(x + 3, cy);
    screen->set_foreground(prev);
    screen->set_drawmode(prev_mode);
}

static void draw_hint_tri_scroll(struct screen *screen, int x, int cy, unsigned fg)
{
    unsigned prev = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(fg);
    /* Up arrow */
    screen->drawpixel(x + 2, cy - 4);
    screen->hline(x + 1, x + 3, cy - 3);
    screen->hline(x, x + 4, cy - 2);
    /* Down arrow */
    screen->hline(x, x + 4, cy + 2);
    screen->hline(x + 1, x + 3, cy + 3);
    screen->drawpixel(x + 2, cy + 4);
    screen->set_foreground(prev);
    screen->set_drawmode(prev_mode);
}

static void draw_capsule_label(struct screen *screen, int x, int y, int w, int h,
                               const char *label, int font)
{
    int old_font = (*screen->current_viewport)->font;
    if (font >= 0)
        screen->setfont(font);
    int lw = 0, lh = 0;
    screen->getstringsize((const unsigned char *)label, &lw, &lh);
    struct viewport *cur = *screen->current_viewport;
    struct viewport tvp = *cur;
    tvp.x = cur->x + x + 6;
    tvp.y = cur->y + y + (h - lh) / 2 - 1;
    tvp.width = MAX(1, w - 12);
    tvp.height = lh + 1;
    struct viewport *prev = screen->set_viewport(&tvp);
    int lx = lw < tvp.width ? (tvp.width - lw) / 2 : 0;
    dialog_draw_shadow_text(screen, lx, 1, label,
                            LCD_RGBPACK(255, 255, 255),
                            LCD_RGBPACK(5, 12, 26), -1);
    screen->set_viewport(prev);
    if (font >= 0)
        screen->setfont(old_font);
}

static void draw_key_legend(struct screen *screen, int x, int y, int w,
                            const char *hint, int arrow_dir, int hint_font)
{
    int old_font = (*screen->current_viewport)->font;
    unsigned prev_fg = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    unsigned hint_fg = LCD_RGBPACK(164, 186, 220);

    if (hint_font >= 0)
        screen->setfont(hint_font);
    int hw = 0, hh = 0;
    if (hint && *hint)
        screen->getstringsize((const unsigned char *)hint, &hw, &hh);
    else
        hh = font_get((*screen->current_viewport)->font)->height;

    int arrow_w = arrow_dir != 0 ? (hw > 0 ? 8 : 4) : 0;
    int total_w = hw + arrow_w;
    int hx = x + MAX(0, (w - total_w) / 2);
    int cy = y + hh / 2;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(hint_fg);
    if (arrow_dir < 0)
    {
        draw_hint_tri_left(screen, hx, cy, hint_fg);
        if (hint && *hint)
            screen->putsxy(hx + 8, y, (const unsigned char *)hint);
    }
    else if (arrow_dir > 0)
    {
        if (hint && *hint)
            screen->putsxy(hx, y, (const unsigned char *)hint);
        draw_hint_tri_right(screen, hx + hw + (hw > 0 ? 4 : 0), cy, hint_fg);
    }
    else if (hint && *hint)
    {
        screen->putsxy(hx, y, (const unsigned char *)hint);
    }

    screen->set_foreground(prev_fg);
    screen->set_drawmode(prev_mode);
    screen->setfont(old_font);
}

void dialog_draw_popup_pair_footer(struct screen *screen, int x, int y,
                                   int w, int h,
                                   const char *left_label,
                                   const char *left_hint, bool left_arrow,
                                   const char *right_label,
                                   const char *right_hint, bool right_arrow,
                                   int label_font, int hint_font)
{
    (void)h;
    if (w <= 32)
        return;
    const int pad = 12;
    const int gap = 8;
    const int bh = 28;
    int bw = (w - 2 * pad - gap) / 2;
    int lx = x + pad;
    int rx = lx + bw + gap;

    draw_alert_capsule(screen, lx, y, bw, bh, false);
    draw_alert_capsule(screen, rx, y, bw, bh, true);
    draw_capsule_label(screen, lx, y, bw, bh, left_label, label_font);
    draw_capsule_label(screen, rx, y, bw, bh, right_label, label_font);

    int hy = y + bh + 4;
    draw_key_legend(screen, lx, hy, bw, left_hint, left_arrow ? -1 : 0, hint_font);
    draw_key_legend(screen, rx, hy, bw, right_hint, right_arrow ? 1 : 0, hint_font);
}

void dialog_draw_popup_single_footer(struct screen *screen, int x, int y,
                                     int w, int h, const char *label,
                                     const char *hint, bool scrollable,
                                     int label_font, int hint_font)
{
    (void)h;
    if (w <= 32)
        return;
    const int bh = 28;
    int bw = MIN(156, w - 24);
    int bx = x + (w - bw) / 2;

    draw_alert_capsule(screen, bx, y, bw, bh, true);
    draw_capsule_label(screen, bx, y, bw, bh, label, label_font);

    int old_font = (*screen->current_viewport)->font;
    unsigned prev_fg = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    unsigned hint_fg = LCD_RGBPACK(164, 186, 220);
    int hy = y + bh + 4;

    if (hint_font >= 0)
        screen->setfont(hint_font);
    int hh = font_get((*screen->current_viewport)->font)->height;
    screen->set_drawmode(DRMODE_FG);
    screen->set_foreground(hint_fg);

    if (scrollable)
    {
        draw_hint_tri_scroll(screen, x + 14, hy + hh / 2, hint_fg);
        screen->putsxy(x + 23, hy, (const unsigned char *)"Scroll");
        if (hint && *hint)
        {
            int hw = 0;
            screen->getstringsize((const unsigned char *)hint, &hw, NULL);
            screen->putsxy(x + w - 14 - hw, hy, (const unsigned char *)hint);
        }
    }
    else if (hint && *hint)
    {
        draw_key_legend(screen, x + 12, hy, w - 24, hint, 0, hint_font);
    }

    screen->set_foreground(prev_fg);
    screen->set_drawmode(prev_mode);
    screen->setfont(old_font);
}

void dialog_draw_popup_action_row(struct screen *screen, int x, int y,
                                  int w, int h, const char *key_text,
                                  bool key_arrow_right,
                                  const char *action_text, bool primary,
                                  int key_font, int label_font)
{
    draw_alert_capsule(screen, x, y, w, h, primary);

    int old_font = (*screen->current_viewport)->font;
    unsigned prev_fg = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    unsigned key_fg = primary ? LCD_RGBPACK(196, 214, 242)
                              : LCD_RGBPACK(164, 186, 220);

    screen->set_drawmode(DRMODE_FG);
    if (key_font >= 0)
        screen->setfont(key_font);
    int kh = 0;
    screen->getstringsize((const unsigned char *)key_text, NULL, &kh);
    int kx = x + 10;
    int ky = y + (h - kh) / 2;
    screen->set_foreground(key_fg);
    if (key_arrow_right)
    {
        draw_hint_tri_right(screen, kx, y + h / 2, key_fg);
        kx += 8;
    }
    screen->putsxy(kx, ky, (const unsigned char *)key_text);

    if (label_font >= 0)
        screen->setfont(label_font);
    int lw = 0, lh = 0;
    screen->getstringsize((const unsigned char *)action_text, &lw, &lh);
    int ax = x + MAX(74, (w - lw) / 2);
    if (ax + lw > x + w - 8)
        ax = x + w - 8 - lw;
    dialog_draw_shadow_text(screen, ax, y + (h - lh) / 2, action_text,
                            LCD_RGBPACK(255, 255, 255),
                            LCD_RGBPACK(5, 12, 26), -1);

    screen->set_foreground(prev_fg);
    screen->set_drawmode(prev_mode);
    screen->setfont(old_font);
}

static void fill_selection_gradient(struct screen *screen, int x, int y,
                                    int w, int h)
{
    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    const struct ui_theme_gradient *grad = &theme.gradient[UI_GRADIENT_SELECTION];
    uint32_t fallback = ui_theme_color(&theme, UI_COLOR_SELECTION, &palette);
    unsigned prev_fg = screen->get_foreground();
    int prev_mode = (*screen->current_viewport)->drawmode;
    screen->set_drawmode(DRMODE_FG);
    for (int row = 0; row < h; row++)
    {
        uint32_t rgb = ui_theme_gradient_at(grad, row, h, fallback);
        screen->set_foreground(LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255));
        screen->hline(x, x + w - 1, y + row);
    }
    screen->set_foreground(prev_fg);
    screen->set_drawmode(prev_mode);
}

void dialog_draw_progress(struct screen *screen, int x, int y, int w, int h,
                          int current, int total)
{
    if (w <= 2 || h <= 2)
        return;
    if (total <= 0)
        total = 1;
    if (current < 0)
        current = 0;
    if (current > total)
        current = total;

    struct ui_theme theme;
    struct ui_album_palette palette;
    ui_theme_snapshot(&theme);
    album_palette_snapshot(&palette);
    struct gloss_fill fill = theme_selection_fill(&theme, &palette);
    int ox, oy;
    struct gloss_surface s = vp_gloss_surface(screen, &ox, &oy);
    int gx = ox + x, gy = oy + y;
    int r = MIN(h / 2, 6);

    gloss_rect(&s, gx, gy, w, h, r, r, &classic_track_dark);
    int fill_w = (w - 2) * current / total;
    if (fill_w > 0)
    {
        struct gloss_surface clip = s;
        clip.clip_x = MAX(s.clip_x, gx + 1);
        clip.clip_w = MAX(0, MIN(s.clip_x + s.clip_w, gx + 1 + fill_w) - clip.clip_x);
        gloss_rect(&clip, gx + 1, gy + 1, MAX(fill_w, 2 * r), h - 2,
                   r > 1 ? r - 1 : 0, r > 1 ? r - 1 : 0, &fill);
    }
    gloss_outline(&s, gx, gy, w, h, r, 0x44ffffff);
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
        if (dialog_is_classic() && style->glass)
        {
            struct frame_buffer_t *fb = box->buffer ? box->buffer
                                                    : &lcd_framebuffer_default;
            bool opaque = fb == &lcd_framebuffer_default ||
                          (fb->stride == box->width && box->width < LCD_WIDTH);
            dialog_draw_glass_panel(screen, 0, 0, box->width, box->height,
                                    DIALOG_GLASS_RADIUS, opaque);
            screen->set_foreground(fg);
            screen->set_drawmode(DRMODE_SOLID);
        }
        else if (dialog_is_classic() && style->popup)
        {
            struct frame_buffer_t *fb = box->buffer ? box->buffer
                                                    : &lcd_framebuffer_default;
            bool opaque = fb == &lcd_framebuffer_default ||
                          (fb->stride == box->width && box->width < LCD_WIDTH);
            dialog_draw_popup_panel(screen, 0, 0, box->width, box->height,
                                    opaque);
            screen->set_foreground(fg);
            screen->set_drawmode(DRMODE_SOLID);
        }
        else if (dialog_is_classic() && bw == 0)
        {
            screen->set_drawmode(DRMODE_FG);
            screen->set_foreground(bg);
            screen->fillrect(0, 0, box->width, box->height);
            screen->set_foreground(fg);
            screen->set_drawmode(DRMODE_SOLID);
        }
        else
        {
            screen->set_drawmode(DRMODE_FG);
            screen->set_foreground(bg);          /* fill with the background colour */
            screen->fillrect(0, 0, box->width, box->height);
            screen->set_foreground(bc);
            draw_box_border(screen, box->width, box->height, bw);
            screen->set_foreground(fg);          /* interior draws in the box fg */
            screen->set_drawmode(DRMODE_SOLID);
        }

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

    if (colour && dialog_is_classic())
    {
        box_fg = screen->get_foreground();
        if (align == DIALOG_BTN_LEFT && bw == 0)
        {
            /* List row inside search/folder dialogs: stock Aqua selection bar. */
            if (selected)
                fill_selection_gradient(screen, x, y, w, h);
            fill = selected ? resolve_fg(screen, style->button_bg_selected)
                            : resolve_bg(screen, style->button_bg);
            text = selected ? LCD_RGBPACK(255, 255, 255)
                            : (style->glass ? LCD_RGBPACK(232, 237, 242)
                                            : LCD_RGBPACK(0, 0, 0));
        }
        else if (style->popup)
        {
            draw_alert_capsule(screen, x, y, w, h, selected);
            fill = selected ? LCD_RGBPACK(56, 98, 152)
                            : LCD_RGBPACK(25, 48, 86);
            text = LCD_RGBPACK(255, 255, 255);
        }
        else
        {
            struct ui_theme theme;
            struct ui_album_palette palette;
            ui_theme_snapshot(&theme);
            album_palette_snapshot(&palette);
            struct gloss_fill blue = theme_selection_fill(&theme, &palette);
            int ox, oy;
            struct gloss_surface s = vp_gloss_surface(screen, &ox, &oy);
            int gx = ox + x, gy = oy + y;
            int cr = r > 0 ? r : 5;
            if (selected)
            {
                gloss_rect(&s, gx, gy, w, h, cr, cr, &blue);
                if (w > 4 && h > 6)
                    gloss_rect(&s, gx + 1, gy + 1, w - 2, (h - 2) / 2,
                               cr > 1 ? cr - 1 : 0, 0, &classic_pill_gloss);
                gloss_outline(&s, gx, gy, w, h, cr, 0xff124c8e);
            }
            else
            {
                gloss_rect(&s, gx, gy, w, h, cr, cr,
                           style->glass ? &classic_well_dark : &classic_well_light);
                gloss_outline(&s, gx, gy, w, h, cr,
                              style->glass ? 0x44ffffff : 0xdc2c2c2e);
            }
            fill = selected ? resolve_fg(screen, style->button_bg_selected)
                            : LCD_RGBPACK(54, 56, 60);
            text = LCD_RGBPACK(255, 255, 255);
        }
    }
    else if (colour)
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
    int tx = MAX((content->width - w) / 2, 0);
    if (dialog_is_classic() && content->fg_pattern == LCD_RGBPACK(255, 255, 255))
        dialog_draw_shadow_text(screen, tx, 0, title,
                                LCD_RGBPACK(255, 255, 255),
                                LCD_RGBPACK(0, 0, 0), -1);
    else
        screen->putsxy(tx, 0, title);
    content->font = body_font;
    unsigned prev_fg = screen->get_foreground();
    if (dialog_is_classic())
        screen->set_foreground(content->fg_pattern == LCD_RGBPACK(255, 255, 255)
                               ? LCD_RGBPACK(80, 86, 96)
                               : LCD_RGBPACK(208, 208, 208));
    screen->hline(0, content->width - 1,
                  dialog_title_height(title) - DIALOG_TITLE_GAP - 1);
    screen->set_foreground(prev_fg);
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

enum dialog_view_mode
{
    DIALOG_VIEW_FLAT = 0,
    DIALOG_VIEW_POPUP,
    DIALOG_VIEW_GLASS,
    DIALOG_VIEW_PAGE,
};

struct dialog_view
{
    struct ui_sheet sheet;
    struct ui_rect bounds;
    struct dialog *dialog;
    struct dialog_view *parent;
    unsigned int thread;
    int handle;
    int status_h;
    int top;
    enum dialog_view_mode mode;
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
    view->sheet.light = view->mode == DIALOG_VIEW_PAGE ? 0 : UI_FULL_OPACITY;
}

static void dialog_capture_chrome(struct dialog_view *view,
                                  const struct dialog *d)
{
    fb_data *body = view->pixels + PANEL_PIXELS;
    int top = view->mode == DIALOG_VIEW_PAGE ? view->status_h : view->top;
    if (top <= 0)
        return;
    if (d->title || view->mode == DIALOG_VIEW_PAGE)
    {
        if (view->mode == DIALOG_VIEW_PAGE)
            sb_invalidate();
        sb_settle(SCREEN_MAIN);
        memcpy(body, lcd_framebuffer_default.fb_ptr,
               (size_t)LCD_WIDTH * top * sizeof(fb_data));
        memcpy(lcd_framebuffer_default.fb_ptr, view->pixels,
               (size_t)LCD_WIDTH * top * sizeof(fb_data));
        ui_take_dirty(SCREEN_MAIN);
    }
    else
        memcpy(body, view->pixels, (size_t)LCD_WIDTH * top * sizeof(fb_data));
}

static struct dialog_view *dialog_view_open(struct dialog *d,
                                            enum dialog_view_mode mode)
{
    if (d->fb && (d->fb_w <= 0 || d->fb_h <= 0 || d->fb->stride != d->fb_w ||
                  d->fb_w * d->fb_h != PANEL_PIXELS || !d->present))
        return NULL;
    int handle = core_alloc_free_space(sizeof(struct dialog_view) + 2 * FRAMEBUFFER_SIZE);
    if (handle <= 0)
        return NULL;

    int sh = sb_visible_height();
    if (sh <= 0)
        sh = (int)ui_theme_metric(UI_METRIC_STATUS_HEIGHT);
    if (sh > LCD_HEIGHT)
        sh = LCD_HEIGHT;

    struct dialog_view *view = core_get_data_pinned(handle);
    *view = (struct dialog_view){
        .handle = handle,
        .dialog = d,
        .parent = active_dialog,
        .thread = thread_self(),
        .mode = mode,
        .status_h = sh,
        .top = (mode == DIALOG_VIEW_GLASS && d->title) ? sh : 0,
    };
    if (mode == DIALOG_VIEW_PAGE)
        view->sheet.slide = true;
    else if (mode == DIALOG_VIEW_POPUP)
    {
        view->sheet.zoom = true;
        view->sheet.bounce = true;
    }
    else if (mode == DIALOG_VIEW_GLASS)
        view->sheet.zoom = true;
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
        dialog_capture_chrome(view, d);
        redraw = true;
    }
    fb_data *body = view->pixels + PANEL_PIXELS;
    if (view->mode == DIALOG_VIEW_PAGE)
    {
        if (redraw)
        {
            uint32_t start = USEC_TIMER;
            int sh = view->status_h;
            fb_data white = LCD_RGBPACK(255, 255, 255);
            for (int i = sh * LCD_WIDTH; i < PANEL_PIXELS; i++)
                body[i] = white;
            struct viewport box = dialog_measure(d);
            if (box.y < sh)
            {
                box.height -= sh - box.y;
                box.y = sh;
            }
            if (box.width > 0 && box.height > 0)
            {
                if (box.x + box.width > LCD_WIDTH)
                    box.width = LCD_WIDTH - box.x;
                if (box.y + box.height > LCD_HEIGHT)
                    box.height = LCD_HEIGHT - box.y;
                struct frame_buffer_t buffer = {
                    .fb_ptr = body,
                    .stride = LCD_WIDTH,
                    .elems = PANEL_PIXELS,
                };
                box.buffer = &buffer;
                dialog_draw_content(d, &screens[SCREEN_MAIN], &box);
            }
            view->bounds = (struct ui_rect){ 0, sh, LCD_WIDTH, LCD_HEIGHT - sh };
            view->sheet.light = -1;
            if (!view->sheet.starting)
                ui_note_render((uint32_t)USEC_TIMER - start);
        }
        ui_sheet_paint_slide(&view->sheet, view->pixels, body, view->status_h);
        return;
    }
    if (view->mode == DIALOG_VIEW_GLASS || view->mode == DIALOG_VIEW_POPUP)
    {
        if (redraw)
        {
            uint32_t start = USEC_TIMER;
            int top = view->top;
            ui_sheet_dim(body + top * LCD_WIDTH, view->pixels + top * LCD_WIDTH,
                         PANEL_PIXELS - top * LCD_WIDTH);
            struct viewport box = dialog_measure(d);
            if (box.width <= 0 || box.height <= 0 ||
                box.width > LCD_WIDTH || box.height > LCD_HEIGHT)
            {
                if (view->bounds.width > 0)
                    dialog_view_restore(view);
                ui_sheet_cancel(&view->sheet);
                view->bounds = (struct ui_rect){0};
                return;
            }
            struct frame_buffer_t buffer = {
                .fb_ptr = body,
                .stride = LCD_WIDTH,
                .elems = PANEL_PIXELS,
            };
            box.buffer = &buffer;
            dialog_draw_content(d, &screens[SCREEN_MAIN], &box);
            int sx = MAX(0, box.x - DIALOG_SHADOW_PAD_X);
            int sy = MAX(top, box.y - DIALOG_SHADOW_PAD_TOP);
            int sx2 = MIN(LCD_WIDTH, box.x + box.width + DIALOG_SHADOW_PAD_X);
            int sy2 = MIN(LCD_HEIGHT, box.y + box.height + DIALOG_SHADOW_PAD_BOT);
            view->bounds = (struct ui_rect){
                sx, sy, MAX(0, sx2 - sx), MAX(0, sy2 - sy)
            };
            view->sheet.light = -1;
            if (!view->sheet.starting)
                ui_note_render((uint32_t)USEC_TIMER - start);
        }
        ui_sheet_paint_zoom(&view->sheet, view->pixels, body, view->bounds,
                            view->top);
        return;
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
            .fb_ptr = body,
            .stride = box.width,
            .elems = (size_t)box.width * box.height,
        };
        view->bounds = (struct ui_rect){box.x, box.y, box.width, box.height};
        box.x = box.y = 0;
        box.buffer = &buffer;
        dialog_draw_content(d, &screens[SCREEN_MAIN], &box);
        if (!view->sheet.starting)
            ui_note_render((uint32_t)USEC_TIMER - start);
    }
    if (d->fb)
        ui_sheet_paint_in(&view->sheet, view->pixels, body,
                          view->bounds, redraw, dialog_output(d), d->fb_w, d->fb_h, d->present);
    else
        ui_sheet_paint(&view->sheet, view->pixels, body,
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

static const struct button_mapping *dialog_fruit_std_keymap(int context)
{
#if BUTTON_REMOTE != 0
    if (context & CONTEXT_REMOTE)
        return get_context_mapping(CONTEXT_STD | CONTEXT_REMOTE);
#endif
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,                BUTTON_NONE },
        { ACTION_STD_PREVREPEAT, BUTTON_SCROLL_BACK|BUTTON_REPEAT,  BUTTON_NONE },
        { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,                 BUTTON_NONE },
        { ACTION_STD_NEXTREPEAT, BUTTON_SCROLL_FWD|BUTTON_REPEAT,   BUTTON_NONE },
        { ACTION_STD_OK,         BUTTON_SELECT|BUTTON_REL,          BUTTON_SELECT },
        { ACTION_STD_CONTEXT,    BUTTON_SELECT|BUTTON_REPEAT,       BUTTON_SELECT },
        { ACTION_STD_CANCEL,     BUTTON_MENU|BUTTON_REL,            BUTTON_MENU },
        { ACTION_WPS_PLAY,       BUTTON_PLAY|BUTTON_REL,            BUTTON_PLAY },
        { ACTION_WPS_SKIPPREV,   BUTTON_LEFT|BUTTON_REL,            BUTTON_LEFT },
        { ACTION_WPS_SKIPNEXT,   BUTTON_RIGHT|BUTTON_REL,           BUTTON_RIGHT },
        LAST_ITEM_IN_LIST
    };
    return keys;
}

bool dialog_handle_playback(int action)
{
    switch (action)
    {
        case ACTION_WPS_PLAY:
            if (audio_status() & AUDIO_STATUS_PLAY)
            {
                if (audio_status() & AUDIO_STATUS_PAUSE)
                    unpause_action(true);
                else
                    pause_action(true);
            }
            return true;
        case ACTION_WPS_SKIPPREV:
            if (audio_status() & AUDIO_STATUS_PLAY)
                audio_prev();
            return true;
        case ACTION_WPS_SKIPNEXT:
            if (audio_status() & AUDIO_STATUS_PLAY)
                audio_next();
            return true;
        default:
            return false;
    }
}

int dialog_run(struct dialog *d, int poll_ticks)
{
    uint32_t requested_at = USEC_TIMER;
    int disp = DIALOG_CONTINUE;
    int font = -1;
    bool outer_inhibit = ui_flush_inhibited();
    bool classic = dialog_is_classic();
    bool fruit_keys = ui_theme_style(UI_STYLE_MENU_TREE) == UI_MENU_TREE_STOCK;
    enum dialog_view_mode mode = DIALOG_VIEW_FLAT;
    if (classic && !d->fb)
    {
        if (d->glass)
            mode = DIALOG_VIEW_GLASS;
        else if (d->full_page)
            mode = DIALOG_VIEW_PAGE;
        else
            mode = DIALOG_VIEW_POPUP;
    }

    if (classic && (d->glass || d->fb) && d->default_style)
        dialog_style_glass(&d->style);
    else if (mode == DIALOG_VIEW_PAGE)
    {
        d->style.popup = false;
        d->style.box_border_width = 0;
        d->style.box_fg = LCD_RGBPACK(0, 0, 0);
        d->style.box_bg = LCD_RGBPACK(255, 255, 255);
    }

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
    bool was_split = menu_pane_split() != 0;
    bool touch_chrome = !d->fb && mode != DIALOG_VIEW_POPUP &&
                        (d->title != NULL || mode == DIALOG_VIEW_PAGE);
    struct dialog_view *view = NULL;
    if (mode != DIALOG_VIEW_FLAT)
    {
        /* Capture the parent screen before changing the status bar title or
         * split mode so the transition starts from the parent's exact pixels. */
        view = dialog_view_open(d, mode);
        if (mode == DIALOG_VIEW_PAGE && !d->fb)
            menu_pane_page(false);
        if (touch_chrome && d->title)
            sb_set_persistent_title(d->title, Icon_NOICON, SCREEN_MAIN);
        if (view && touch_chrome)
            dialog_capture_chrome(view, d);
        else if (!view && touch_chrome)
        {
            if (mode == DIALOG_VIEW_PAGE)
                sb_invalidate();
            sb_settle(SCREEN_MAIN);
        }
    }
    else
    {
        if (d->title && !d->fb)
        {
            sb_set_persistent_title(d->title, Icon_NOICON, SCREEN_MAIN);
            sb_settle(SCREEN_MAIN);
        }
        view = dialog_view_open(d, DIALOG_VIEW_FLAT);
    }
    d->poll_ticks = poll_ticks;
    /* Wheel events have no release. A USB popup opened after turning the
     * wheel must accept the next key instead of waiting for one. */
    if (button_status() & ~(BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD))
        action_wait_for_release_preserve_queue();

    if (!d->motion_route)
        d->motion_route = d->context == CONTEXT_KEYBOARD ? "keyboard" : "dialog";
    if (view)
    {
        ui_sheet_begin(&view->sheet, true, d->motion_route);
        view->sheet.requested_at = requested_at;
    }

    const struct button_mapping *(*active_keymap)(int) = d->keymap;
    if (!active_keymap && fruit_keys &&
        (d->context == CONTEXT_STD || d->context == CONTEXT_LIST))
        active_keymap = dialog_fruit_std_keymap;

    bool redraw = true;
    int ticks = d->poll_ticks;
    while (disp == DIALOG_CONTINUE)
    {
        unsigned palette_before = album_palette_generation();
        album_palette_refresh();
        redraw |= palette_before != album_palette_generation();
        if (redraw && d->default_style)
        {
            if (classic && (d->glass || d->fb))
                dialog_style_glass(&d->style);
            else
            {
                dialog_style_default(&d->style);
                if (mode == DIALOG_VIEW_PAGE)
                {
                    d->style.popup = false;
                    d->style.box_border_width = 0;
                    d->style.box_fg = LCD_RGBPACK(0, 0, 0);
                    d->style.box_bg = LCD_RGBPACK(255, 255, 255);
                }
            }
        }
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
        int action = active_keymap
                   ? get_custom_action(CONTEXT_PLUGIN, wake, active_keymap)
                   : get_action(d->context, wake);
        ui_inhibit_flush(was_inhibited);

        if (view && ui_take_dirty(SCREEN_MAIN))
            view->sheet.light = -1;
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
        if (fruit_keys && !d->keymap && dialog_handle_playback(action))
        {
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
    if (mode == DIALOG_VIEW_PAGE && !d->fb && was_split)
    {
        menu_pane_page(true);
        sb_invalidate();
    }
    if (touch_chrome && (d->title || (mode == DIALOG_VIEW_PAGE && was_split)))
    {
        if (d->title)
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
