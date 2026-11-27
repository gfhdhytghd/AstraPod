/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/quickscreen.c
 * Copyright (C) 2008 by Jonathan Gordon
 * GNU General Public License (version 2+)
 *
 * The quick screen: four settings bound to the directional buttons, edited
 * in place without leaving playback.
 ****************************************************************************/

#include <stdio.h>
#include "config.h"
#include "system.h"
#include "core_alloc.h"
#include "font.h"
#include "kernel.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "system/volume.h"
#include "sound.h"
#include "input/action.h"
#include "settings/settings_list.h"
#include "lang.h"
#include "playlist/playlist.h"
#include "draw/viewport.h"
#include "audio.h"
#include "quick_screen.h"
#include "cider.h"
#include "cider_controls.h"
#include "cider_wps.h"
#include "speech/talk.h"
#include "widgets/list.h"
#include "widgets/option_select.h"
#include "debug.h"
#include "draw/gloss.h"
#include "screens/shortcuts.h"
#include "system/appevents.h"
#include "metadata/album_palette.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/scene.h"
#include "ui/sheet.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include <string.h>
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif

#define QUICK_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define CC_MAX_ROWS (1 + QUICKSCREEN_ITEM_COUNT)
#define CC_FOCUS_TIMEOUT (4 * HZ)

struct gui_quickscreen;
struct quickscreen_card {
    struct ui_view view;
    struct gui_quickscreen *qs;
    enum quickscreen_item item;
};

struct cc_row {
    struct ui_view view;
    struct gui_quickscreen *qs;
    int index;
    enum quickscreen_item item; /* QUICKSCREEN_ITEM_COUNT for Volume hero */
    uint8_t key_mask;
};

struct quickscreen_page {
    struct ui_theme theme;
    struct ui_album_palette palette;
    struct ui_nav nav;
    struct ui_page page;
    struct ui_view background;
    struct ui_sheet sheet;
    struct frame_buffer_t tile;
    struct frame_buffer_t *paint_target;
    fb_data *underneath;
    int sheet_handle;
    struct quickscreen_card cards[QUICKSCREEN_ITEM_COUNT];
    struct cc_row rows[CC_MAX_ROWS];
    int row_count;
    unsigned palette_generation;
    int title_font, body_font, fallback_font;
    char saved_title[MAX_PATH];
    enum themable_icons saved_icon;
    bool has_saved_title;
    bool full_page;         /* style.quick_screen=page */
    bool wheel;             /* ui_wheel_navigation(): wheel/SELECT/MENU */
};

struct gui_quickscreen
{
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    struct quickscreen_page *view;
    int view_handle;
    int result;
    int volume_item;
    int focus_row;
    long focus_tick;
    bool cider;
    void (*poll)(void);  /* Keep a caller's foreground inbox flowing. */
};

/* Cider mode shown in a slot of a Cider quick screen */
static int cider_slot_mode(enum quickscreen_item i)
{
    return i == QUICKSCREEN_LEFT ? CIDER_MODE_SHUFFLE : CIDER_MODE_REPEAT;
}

static bool cider_slot(const struct gui_quickscreen *qs, enum quickscreen_item i)
{
    return qs->cider && (i == QUICKSCREEN_LEFT || i == QUICKSCREEN_RIGHT);
}

static const char *quickscreen_title(struct gui_quickscreen *qs,
                                     enum quickscreen_item i)
{
    return cider_slot(qs, i) ? cider_mode_title(cider_slot_mode(i))
                     : P2STR(ID2P(qs->items[i]->lang_id));
}

static const char *quickscreen_value(struct gui_quickscreen *qs,
                                     enum quickscreen_item item,
                                     char *buffer, size_t size)
{
    if (cider_slot(qs, item))
        return cider_mode_label(cider_slot_mode(item),
                                cider_mode_get(cider_slot_mode(item)));
    const struct settings_list *setting = qs->items[item];
    return option_get_valuestring(setting, buffer, size, option_value_as_int(setting));
}

static unsigned quickscreen_color(const struct quickscreen_page *view, enum ui_color_role role)
{
    uint32_t rgb = ui_theme_color(&view->theme, role, &view->palette);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static struct viewport quickscreen_viewport(const struct quickscreen_page *view,
                                               const struct ui_rect *clip, int font)
{
    return (struct viewport){
        .x = clip->x, .y = clip->y, .width = clip->width, .height = clip->height,
        .font = font, .drawmode = DRMODE_FG, .flags = VP_DEFAULT_FLAGS,
        .buffer = view->paint_target,
    };
}

static void quickscreen_paint_background(const struct ui_view *node,
                                          const struct ui_view_frame *frame)
{
    const struct quickscreen_page *view = node->data;
    struct viewport vp = quickscreen_viewport(view, &frame->clip, view->fallback_font);
    vp.fg_pattern = quickscreen_color(view, UI_COLOR_BACKGROUND);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_viewport(previous);
}

/* The wheel uses the four physical directions. Paint their cues with two
 * lines each so they do not depend on an icon font or a theme icon strip. */
static void quickscreen_paint_arrow(enum quickscreen_item item, int x, int y, int size)
{
    int dx = (item == QUICKSCREEN_LEFT) ? -size : (item == QUICKSCREEN_RIGHT) ? size : 0;
    int dy = (item == QUICKSCREEN_TOP) ? -size : (item == QUICKSCREEN_BOTTOM) ? size : 0;
    lcd_drawline(x + dy, y + dx, x + dx, y + dy);
    lcd_drawline(x - dy, y - dx, x + dx, y + dy);
}

static void quickscreen_paint_card(const struct ui_view *node,
                                    const struct ui_view_frame *frame)
{
    struct quickscreen_card *card = node->data;
    struct quickscreen_page *view = card->qs->view;
    const struct ui_rect *box = &frame->bounds;
    int title_font = view->title_font >= 0 ? view->title_font : view->fallback_font;
    int body_font = view->body_font >= 0 ? view->body_font : view->fallback_font;
    struct viewport vp = quickscreen_viewport(view, &frame->clip, title_font);
    int x = box->x - vp.x, y = box->y - vp.y;
    int padding = MIN(view->theme.metric[UI_METRIC_MODAL_PADDING], box->width / 4);
    int title_height = font_get(title_font)->height;
    int body_height = font_get(body_font)->height;
    int text_top = MAX(0, (box->height - title_height - body_height) / 2);
    int arrow_space = MIN(padding + body_height, box->width / 3);
    char buffer[MAX_PATH];

    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(quickscreen_color(view, UI_COLOR_MODAL_BG));
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_foreground(quickscreen_color(view, UI_COLOR_SEPARATOR));
    lcd_drawrect(x, y, box->width, box->height);
    lcd_set_foreground(quickscreen_color(view, UI_COLOR_ACCENT));
    quickscreen_paint_arrow(card->item, x + box->width - arrow_space / 2,
                            y + box->height / 2, MAX(2, MIN(arrow_space / 4, body_height / 4)));
    lcd_set_viewport(previous);

    struct ui_rect text = frame->clip;
    int end = MIN(text.x + text.width, box->x + box->width - arrow_space);
    text.x = MAX(text.x, box->x + padding);
    text.width = end - text.x;
    if (text.width <= 0)
        return;

    vp = quickscreen_viewport(view, &text, title_font);
    previous = lcd_set_viewport(&vp);
    int tx = box->x + padding - vp.x;
    int ty = box->y + text_top - vp.y;
    lcd_set_foreground(quickscreen_color(view, UI_COLOR_MODAL_FG));
    lcd_putsxy(tx, ty, quickscreen_title(card->qs, card->item));
    lcd_setfont(body_font);
    lcd_set_foreground(quickscreen_color(view, UI_COLOR_MUTED));
    lcd_putsxy(tx, ty + title_height, quickscreen_value(card->qs, card->item, buffer, sizeof(buffer)));
    lcd_set_viewport(previous);
}

/* Full-screen Control Centre page: Volume hero at top followed by the
 * configured settings as full-width skeuomorphic rows under the stock status
 * bar, with glossy sliders, iPhone OS switches, Aqua stepper pills, and
 * Classic's brushed-chrome directional key caps. */
#define CC_PAD          14
#define CC_KEY_X        10
#define CC_SWITCH_W     90
#define CC_SWITCH_KNOB  34
#define CC_SWITCH_R     6
#define CC_PILL_MIN_W   92
#define CC_PILL_MAX_W   148

#define CC_LABEL_RGB        0x111418
#define CC_LABEL_SEL_RGB    0xffffff
#define CC_VALUE_RGB        0x0060c8
#define CC_SHADOW_SEL_RGB   0x003670
#define CC_SHADOW_LIGHT_RGB 0xffffff
#define CC_ROW_SHADE_RGB    0xcfd5dd

static const struct gloss_fill cc_backdrop_fill = {
    0xff92a2b2, 0xff8c9dad, 0xff525f6d, 0xff46525e };
static const struct gloss_fill cc_row_fill = {
    0xffffffff, 0xfffcfdff, 0xffeaeef3, 0xffaeb6c0 };
static const struct gloss_fill cc_key_fill = {
    0xffffffff, 0xfff3f5f8, 0xffcfd5dd, 0xffb4bcc6 };
static const struct gloss_fill cc_key_gloss = {
    0x60ffffff, 0x38ffffff, 0x10ffffff, 0x00ffffff };
static const struct gloss_fill cc_knob_fill = {
    0xffffffff, 0xfff8f9fb, 0xffc4c9d0, 0xff949aa4 };
static const struct gloss_fill cc_slider_off = {
    0xff8c939d, 0xffcfd4dc, 0xffeef1f4, 0xffffffff };
static const struct gloss_fill cc_slider_on_sel = {
    0xffffffff, 0xffdcf3ff, 0xff92d3ff, 0xff5eaee8 };
static const struct gloss_fill cc_slider_off_sel = {
    0xff002757, 0xff003c7d, 0xff0658a8, 0xff3b93dc };
static const struct gloss_fill cc_switch_on_sel = {
    0xff002a5c, 0xff004696, 0xff005fc4, 0xff1c7ee0 };
static const struct gloss_fill cc_switch_off = {
    0xff8a9099, 0xffd2d6dc, 0xfff3f5f8, 0xffffffff };
static const struct gloss_fill cc_switch_gloss = {
    0x48ffffff, 0x24ffffff, 0x08ffffff, 0x00ffffff };
static const struct gloss_fill cc_pill_fill = {
    0xffffffff, 0xfff5f7fa, 0xffd0d6df, 0xffadb5c0 };
static const struct gloss_fill cc_pill_sel = {
    0xffffffff, 0xfff8fbff, 0xffd2e2f4, 0xffa6c2df };
static const struct gloss_fill cc_pill_gloss = {
    0x55ffffff, 0x28ffffff, 0x08ffffff, 0x00ffffff };

static struct gloss_surface cc_surface(const struct quickscreen_page *view,
                                       const struct ui_rect *clip)
{
    return (struct gloss_surface){
        .pixels = view->paint_target->fb_ptr, .stride = LCD_WIDTH,
        .clip_x = clip->x, .clip_y = clip->y,
        .clip_w = clip->width, .clip_h = clip->height };
}

static unsigned cc_pack(uint32_t rgb)
{
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

/* The stock selection bar's blue, as the theme sets its four stops. */
static struct gloss_fill cc_blue(const struct quickscreen_page *view)
{
    const uint32_t *c = view->theme.gradient[UI_GRADIENT_SELECTION].color;
    if (!view->theme.gradient[UI_GRADIENT_SELECTION].stops)
    {
        uint32_t rgb = ui_theme_color(&view->theme, UI_COLOR_SELECTION,
                                      &view->palette);
        return (struct gloss_fill){ 0xff000000 | rgb, 0xff000000 | rgb,
                                    0xff000000 | rgb, 0xff000000 | rgb };
    }
    return (struct gloss_fill){ 0xff000000 | c[0], 0xff000000 | c[1],
                                0xff000000 | c[2], 0xff000000 | c[3] };
}

static struct ui_rect cc_intersect(struct ui_rect a, struct ui_rect b)
{
    int x = MAX(a.x, b.x), y = MAX(a.y, b.y);
    return (struct ui_rect){ x, y,
        MAX(0, MIN(a.x + a.width, b.x + b.width) - x),
        MAX(0, MIN(a.y + a.height, b.y + b.height) - y) };
}

/* `text` with its line's top-left at screen (x, y), over a one-pixel shadow
 * `dy` rows away, clipped to `clip`. */
static void cc_text(const struct quickscreen_page *view, struct ui_rect clip,
                    int font, int x, int y, const char *text, uint32_t rgb,
                    uint32_t shadow, int dy)
{
    if (clip.width <= 0 || clip.height <= 0)
        return;
    struct viewport vp = quickscreen_viewport(view, &clip, font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(cc_pack(shadow));
    lcd_putsxy(x - vp.x, y - vp.y + dy, text);
    lcd_set_foreground(cc_pack(rgb));
    lcd_putsxy(x - vp.x, y - vp.y, text);
    lcd_set_viewport(previous);
}

static int cc_text_width(const char *text, int font)
{
    int w = 0, h = 0;
    font_getstringsize(text, &w, &h, font);
    return w;
}

/* Exposed only when fewer than four rows are shown. */
static void cc_paint_background(const struct ui_view *node,
                                const struct ui_view_frame *frame)
{
    const struct quickscreen_page *view = node->data;
    const struct ui_rect *c = &frame->clip;
    if (view->theme.style[UI_STYLE_ART] == UI_ART_PLASTIC && stock_art_ready())
    {
        fb_data *fb = view->paint_target->fb_ptr;
        for (int y = c->y; y < c->y + c->height; y++)
            stock_art_row(STOCK_ART_BACKGROUND_LIGHTBLUE, c->x, y,
                          fb + y * LCD_WIDTH + c->x, c->width);
        return;
    }
    struct gloss_surface s = cc_surface(view, c);
    gloss_rect(&s, frame->bounds.x, frame->bounds.y,
               frame->bounds.width, frame->bounds.height,
               0, 0, &cc_backdrop_fill);
}

static void cc_paint_row_bg(const struct quickscreen_page *view,
                            const struct ui_rect *box,
                            const struct ui_rect *clip, bool selected)
{
    if (selected)
    {
        const struct ui_theme_gradient *g =
            &view->theme.gradient[UI_GRADIENT_SELECTION];
        uint32_t fallback = ui_theme_color(&view->theme, UI_COLOR_SELECTION,
                                           &view->palette);
        fb_data *fb = view->paint_target->fb_ptr;
        int y0 = MAX(box->y, clip->y);
        int y1 = MIN(box->y + box->height, clip->y + clip->height);
        int x0 = MAX(box->x, clip->x);
        int x1 = MIN(box->x + box->width, clip->x + clip->width);
        for (int y = y0; y < y1; y++)
        {
            fb_data color = cc_pack(ui_theme_gradient_at(g, y - box->y,
                                                         box->height, fallback));
            fb_data *row = fb + y * LCD_WIDTH;
            for (int x = x0; x < x1; x++)
                row[x] = color;
        }
        return;
    }
    struct gloss_surface s = cc_surface(view, clip);
    gloss_rect(&s, box->x, box->y, box->width, box->height, 0, 0, &cc_row_fill);
    int shade_y = box->y + box->height - 2;
    if (shade_y >= clip->y && shade_y < clip->y + clip->height)
    {
        int x0 = MAX(box->x, clip->x);
        int x1 = MIN(box->x + box->width, clip->x + clip->width);
        fb_data *row = view->paint_target->fb_ptr + shade_y * LCD_WIDTH;
        fb_data shade = cc_pack(CC_ROW_SHADE_RGB);
        for (int x = x0; x < x1; x++)
            row[x] = shade;
    }
}

static void cc_paint_speaker_one(int ox, int cy, int waves)
{
    lcd_fillrect(ox, cy - 2, 3, 5);
    for (int i = 0; i < 4; i++)
        lcd_vline(ox + 3 + i, cy - (2 + i), cy + 2 + i);
    if (waves >= 1)
        for (int yy = -3; yy <= 3; yy++)
            lcd_drawpixel(ox + 9 + (abs(yy) <= 1 ? 1 : 0), cy + yy);
    if (waves >= 2)
        for (int yy = -5; yy <= 5; yy++)
            lcd_drawpixel(ox + 12 + (abs(yy) <= 2 ? 1 : 0), cy + yy);
}

static void cc_paint_speaker(const struct quickscreen_page *view,
                             struct ui_rect clip, int x, int cy, int waves,
                             uint32_t rgb, uint32_t shadow, int dy)
{
    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(cc_pack(shadow));
    cc_paint_speaker_one(x - vp.x, cy - vp.y + dy, waves);
    lcd_set_foreground(cc_pack(rgb));
    cc_paint_speaker_one(x - vp.x, cy - vp.y, waves);
    lcd_set_viewport(previous);
}

static void cc_paint_key_glyph(uint8_t mask, enum quickscreen_item item,
                               int cx, int cy)
{
    const uint8_t both_tb = (1u << QUICKSCREEN_TOP) | (1u << QUICKSCREEN_BOTTOM);
    if ((mask & both_tb) == both_tb)
    {
        for (int i = 0; i < 5; i++)
        {
            lcd_hline(cx - i, cx + i, cy - 6 + i);
            lcd_hline(cx - i, cx + i, cy + 6 - i);
        }
        return;
    }
    for (int i = 0; i < 6; i++)
    {
        if (item == QUICKSCREEN_TOP)
            lcd_hline(cx - i, cx + i, cy - 3 + i);
        else if (item == QUICKSCREEN_BOTTOM)
            lcd_hline(cx - i, cx + i, cy + 3 - i);
        else if (item == QUICKSCREEN_LEFT)
            lcd_vline(cx - 3 + i, cy - i, cy + i);
        else
            lcd_vline(cx + 3 - i, cy - i, cy + i);
    }
}

static void cc_paint_key_badge(const struct quickscreen_page *view,
                               struct gloss_surface *s, struct ui_rect clip,
                               int x, int y, int size, uint8_t mask,
                               enum quickscreen_item item)
{
    int r = size < 26 ? 6 : 7;
    gloss_shadow(s, x, y, size, size, r, 2, 1, 70);
    gloss_rect(s, x, y, size, size, r, r, &cc_key_fill);
    gloss_rect(s, x + 1, y + 1, size - 2, size / 2 - 1, r - 1, 0,
               &cc_key_gloss);
    gloss_outline(s, x, y, size, size, r, 0xff8b94a0);

    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    int cx = x + size / 2 - vp.x;
    int cy = y + size / 2 - vp.y;
    lcd_set_foreground(cc_pack(0xffffff));
    cc_paint_key_glyph(mask, item, cx, cy + 1);
    lcd_set_foreground(cc_pack(0x204b7c));
    cc_paint_key_glyph(mask, item, cx, cy);
    lcd_set_viewport(previous);
}

static void cc_knob(struct gloss_surface *s, int x, int y, int w, int h,
                    int r, bool selected)
{
    gloss_shadow(s, x, y, w, h, r, 2, 1, 100);
    gloss_rect(s, x, y, w, h, r, r, &cc_knob_fill);
    gloss_outline(s, x, y, w, h, r, selected ? 0xff003064 : 0xff5a616b);
}

static void cc_slider(const struct quickscreen_page *view,
                      struct gloss_surface *s, int left_x, int right_x,
                      int track_y, int track_h, int knob_d, int position,
                      int count, bool selected)
{
    int left_k = left_x + knob_d / 2;
    int right_k = right_x - knob_d / 2;
    int kx = left_k + (count > 1 ? (right_k - left_k) * position / (count - 1)
                                 : 0);
    int tw = right_x - left_x;
    struct gloss_fill on_fill = selected ? cc_slider_on_sel : cc_blue(view);
    const struct gloss_fill *off_fill =
        selected ? &cc_slider_off_sel : &cc_slider_off;

    struct gloss_surface half = *s;
    half.clip_w = MAX(0, MIN(s->clip_x + s->clip_w, kx) - s->clip_x);
    gloss_rect(&half, left_x, track_y, tw, track_h, track_h / 2, track_h / 2,
               &on_fill);
    half = *s;
    half.clip_x = MAX(s->clip_x, kx);
    half.clip_w = MAX(0, s->clip_x + s->clip_w - half.clip_x);
    gloss_rect(&half, left_x, track_y, tw, track_h, track_h / 2, track_h / 2,
               off_fill);
    gloss_outline(s, left_x, track_y, tw, track_h, track_h / 2,
                  selected ? 0xff002a5c : 0xff6e7782);

    int ky = track_y + track_h / 2 - knob_d / 2;
    cc_knob(s, kx - knob_d / 2, ky, knob_d, knob_d, knob_d / 2, selected);
}

static void cc_switch(const struct quickscreen_page *view,
                      struct gloss_surface *s, struct ui_rect clip,
                      int sx, int sy, int sw_h, bool on, bool selected)
{
    int font = view->body_font >= 0 ? view->body_font : view->fallback_font;
    int fh = font_get(font)->height;
    int ty = sy + (sw_h - fh) / 2 - 1;
    const char *label = str(on ? LANG_ON : LANG_OFF);
    int tw = cc_text_width(label, font);

    if (on)
    {
        struct gloss_fill blue = cc_blue(view);
        gloss_rect(s, sx, sy, CC_SWITCH_W, sw_h, CC_SWITCH_R,
                   CC_SWITCH_R, selected ? &cc_switch_on_sel : &blue);
        gloss_rect(s, sx + 1, sy + 1, CC_SWITCH_W - 2, sw_h / 2 - 1,
                   CC_SWITCH_R - 1, 0, &cc_switch_gloss);
        gloss_outline(s, sx, sy, CC_SWITCH_W, sw_h, CC_SWITCH_R,
                      selected ? 0xc8ffffff : 0xff284f7a);
        struct ui_rect text = cc_intersect(clip, (struct ui_rect){
            sx, sy, CC_SWITCH_W - CC_SWITCH_KNOB, sw_h });
        cc_text(view, text, font,
                sx + (CC_SWITCH_W - CC_SWITCH_KNOB - tw) / 2, ty, label,
                0xffffff, 0x002d64, -1);
        cc_knob(s, sx + CC_SWITCH_W - CC_SWITCH_KNOB, sy, CC_SWITCH_KNOB,
                sw_h, CC_SWITCH_R, selected);
    }
    else
    {
        gloss_rect(s, sx, sy, CC_SWITCH_W, sw_h, CC_SWITCH_R,
                   CC_SWITCH_R, &cc_switch_off);
        gloss_outline(s, sx, sy, CC_SWITCH_W, sw_h, CC_SWITCH_R,
                      selected ? 0xff003064 : 0xff78808a);
        struct ui_rect text = cc_intersect(clip, (struct ui_rect){
            sx + CC_SWITCH_KNOB, sy, CC_SWITCH_W - CC_SWITCH_KNOB, sw_h });
        cc_text(view, text, font,
                sx + CC_SWITCH_KNOB + (CC_SWITCH_W - CC_SWITCH_KNOB - tw) / 2,
                ty, label, 0x525860, 0xffffff, 1);
        cc_knob(s, sx, sy, CC_SWITCH_KNOB, sw_h, CC_SWITCH_R, selected);
    }
}

static void cc_pill(const struct quickscreen_page *view,
                    struct gloss_surface *s, struct ui_rect clip,
                    int px, int py, int pw, int ph, const char *value,
                    bool selected)
{
    int font = view->body_font >= 0 ? view->body_font : view->fallback_font;
    int fh = font_get(font)->height;
    int r = ph < 32 ? 6 : 7;
    gloss_shadow(s, px, py, pw, ph, r, selected ? 2 : 1, 1,
                 selected ? 95 : 55);
    gloss_rect(s, px, py, pw, ph, r, r,
               selected ? &cc_pill_sel : &cc_pill_fill);
    gloss_rect(s, px + 1, py + 1, pw - 2, ph / 2 - 1, r - 1, 0,
               &cc_pill_gloss);
    gloss_outline(s, px, py, pw, ph, r,
                  selected ? 0xff003a7a : 0xff828c98);

    int mx = px + pw - 13, my = py + ph / 2;
    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(cc_pack(selected ? 0x004a99 : 0x3b4858));
    for (int i = 0; i < 4; i++)
    {
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my - 5 + i - vp.y);
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my + 5 - i - vp.y);
    }
    lcd_set_viewport(previous);

    int room = pw - 24;
    int vw = cc_text_width(value, font);
    struct ui_rect text = cc_intersect(clip, (struct ui_rect){
        px + 6, py, room, ph });
    cc_text(view, text, font, px + 7 + (room - MIN(vw, room)) / 2,
            py + (ph - fh) / 2 - 1, value,
            selected ? 0x004a99 : 0x124580, 0xffffff, 1);
}

static void cc_paint_volume_hero(const struct cc_row *row,
                                 const struct ui_view_frame *frame,
                                 bool selected)
{
    struct gui_quickscreen *qs = row->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    struct gloss_surface s = cc_surface(view, &frame->clip);
    int title_font = view->title_font >= 0 ? view->title_font
                                           : view->fallback_font;
    int body_font = view->body_font >= 0 ? view->body_font
                                         : view->fallback_font;
    char buffer[32];
    int position = 0, count = 101;
    if (qs->cider)
    {
        struct cider_status st;
        cider_status(&st);
        position = MIN(MAX(st.volume, 0), 100);
        snprintf(buffer, sizeof(buffer), "%d%%", position);
    }
    else
    {
        int min = sound_min(SOUND_VOLUME), max = sound_max(SOUND_VOLUME);
        int cur = MIN(MAX(global_status.volume, min), max);
        count = MAX(2, max - min + 1);
        position = cur - min;
        format_sound_value(buffer, sizeof(buffer), SOUND_VOLUME, cur);
    }

    uint32_t lbl_rgb = selected ? CC_LABEL_SEL_RGB : CC_LABEL_RGB;
    uint32_t lbl_shd = selected ? CC_SHADOW_SEL_RGB : CC_SHADOW_LIGHT_RGB;
    int lbl_dy = selected ? -1 : 1;
    int top_y = box->y + (box->height >= 60 ? 8 : 4);
    int right = box->x + box->width - CC_PAD;
    int vw = cc_text_width(buffer, body_font);
    int vx = right - vw;
    struct ui_rect lbl_clip = cc_intersect(frame->clip, (struct ui_rect){
        box->x + CC_PAD, box->y, MAX(0, vx - 8 - (box->x + CC_PAD)),
        box->height });
    cc_text(view, lbl_clip, title_font, box->x + CC_PAD, top_y,
            str(LANG_VOLUME), lbl_rgb, lbl_shd, lbl_dy);
    struct ui_rect val_clip = cc_intersect(frame->clip, (struct ui_rect){
        vx - 2, box->y, right - vx + 4, box->height });
    cc_text(view, val_clip, body_font, vx, top_y - 2, buffer,
            selected ? CC_LABEL_SEL_RGB : CC_VALUE_RGB, lbl_shd, lbl_dy);

    int track_h = box->height >= 60 ? 11 : 9;
    int knob_d = box->height >= 60 ? 20 : 18;
    int track_y = box->y + box->height - (box->height >= 60 ? 24 : 20);
    int cy = track_y + track_h / 2;
    uint32_t ico_rgb = selected ? 0xffffff : 0x204b7c;
    cc_paint_speaker(view, frame->clip, box->x + CC_PAD, cy, 0,
                     ico_rgb, lbl_shd, lbl_dy);
    cc_paint_speaker(view, frame->clip, right - 14, cy, 2,
                     ico_rgb, lbl_shd, lbl_dy);
    cc_slider(view, &s, box->x + 28, box->x + box->width - 34,
              track_y, track_h, knob_d, position, count, selected);
}

static void cc_paint_row(const struct ui_view *node,
                         const struct ui_view_frame *frame)
{
    struct cc_row *row = node->data;
    struct gui_quickscreen *qs = row->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    bool selected = row->index == qs->focus_row;

    cc_paint_row_bg(view, box, &frame->clip, selected);
    if (row->item == QUICKSCREEN_ITEM_COUNT)
    {
        cc_paint_volume_hero(row, frame, selected);
        return;
    }

    struct gloss_surface s = cc_surface(view, &frame->clip);
    int title_font = view->title_font >= 0 ? view->title_font
                                           : view->fallback_font;
    int body_font = view->body_font >= 0 ? view->body_font
                                         : view->fallback_font;
    int title_h = font_get(title_font)->height;
    char buffer[MAX_PATH];

    int tx = CC_PAD;
    if (!view->wheel)
    {
        int ksz = box->height >= 48 ? 26 : 24;
        cc_paint_key_badge(view, &s, frame->clip, box->x + CC_KEY_X,
                           box->y + (box->height - ksz) / 2, ksz,
                           row->key_mask, row->item);
        tx = ksz == 26 ? 46 : 44;
    }

    int right = box->x + box->width - CC_PAD;
    uint32_t lbl_rgb = selected ? CC_LABEL_SEL_RGB : CC_LABEL_RGB;
    uint32_t lbl_shd = selected ? CC_SHADOW_SEL_RGB : CC_SHADOW_LIGHT_RGB;
    int lbl_dy = selected ? -1 : 1;

    const struct settings_list *setting = qs->items[row->item];
    const char *title = quickscreen_title(qs, row->item);
    const char *value = quickscreen_value(qs, row->item, buffer,
                                          sizeof(buffer));
    int count = 0, position = 0;

    if (!cider_slot(qs, row->item) &&
        (setting->flags & (F_INT_SETTING | F_T_SOUND)) &&
        get_setting_info_for_bar(setting, 0, &count, &position) && count > 2)
    {
        int vw = cc_text_width(value, body_font);
        int vx = right - vw;
        int top_y = box->y + (box->height >= 48 ? 5 : 3);
        struct ui_rect lbl_clip = cc_intersect(frame->clip, (struct ui_rect){
            box->x + tx, box->y, MAX(0, vx - 8 - (box->x + tx)), box->height });
        cc_text(view, lbl_clip, title_font, box->x + tx, top_y, title,
                lbl_rgb, lbl_shd, lbl_dy);
        struct ui_rect val_clip = cc_intersect(frame->clip, (struct ui_rect){
            vx - 2, box->y, right - vx + 4, box->height });
        cc_text(view, val_clip, body_font, vx, top_y - 2, value,
                selected ? CC_LABEL_SEL_RGB : CC_VALUE_RGB, lbl_shd, lbl_dy);
        int track_h = box->height >= 48 ? 9 : 8;
        int knob_d = box->height >= 48 ? 18 : 16;
        int track_y = box->y + box->height - (box->height >= 48 ? 20 : 16);
        cc_slider(view, &s, box->x + tx, right, track_y, track_h, knob_d,
                  position, count, selected);
        return;
    }

    int ty = box->y + (box->height - title_h) / 2;
    if (!cider_slot(qs, row->item) && (setting->flags & F_T_MASK) == F_T_BOOL)
    {
        int sw_h = box->height < 48 ? 28 : 30;
        int sx = right - CC_SWITCH_W;
        int sy = box->y + (box->height - sw_h) / 2;
        struct ui_rect lbl_clip = cc_intersect(frame->clip, (struct ui_rect){
            box->x + tx, box->y, MAX(0, sx - 8 - (box->x + tx)), box->height });
        cc_text(view, lbl_clip, title_font, box->x + tx, ty, title,
                lbl_rgb, lbl_shd, lbl_dy);
        cc_switch(view, &s, frame->clip, sx, sy, sw_h,
                  option_value_as_int(setting), selected);
        return;
    }

    int vw = cc_text_width(value, body_font);
    int pw = MAX(CC_PILL_MIN_W, MIN(CC_PILL_MAX_W, vw + 36));
    int ph = box->height < 48 ? 28 : 32;
    int px = right - pw;
    int py = box->y + (box->height - ph) / 2;
    struct ui_rect lbl_clip = cc_intersect(frame->clip, (struct ui_rect){
        box->x + tx, box->y, MAX(0, px - 8 - (box->x + tx)), box->height });
    cc_text(view, lbl_clip, title_font, box->x + tx, ty, title,
            lbl_rgb, lbl_shd, lbl_dy);
    cc_pill(view, &s, frame->clip, px, py, pw, ph, value, selected);
}

static void cc_build_rows(struct gui_quickscreen *qs, const struct viewport *vp)
{
    struct quickscreen_page *view = qs->view;
    static const struct ui_view_ops cc_row_ops = {.paint = cc_paint_row};

    view->row_count = 1;
    view->rows[0] = (struct cc_row){
        .qs = qs, .index = 0, .item = QUICKSCREEN_ITEM_COUNT, .key_mask = 0,
    };

    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
    {
        if (!qs->items[i])
            continue;
        if (!cider_slot(qs, i) && qs->items[i]->lang_id == LANG_VOLUME)
        {
            view->rows[0].key_mask |= 1u << i;
            continue;
        }
        int dup = -1;
        if (!cider_slot(qs, i))
            for (int r = 1; r < view->row_count; ++r)
                if (!cider_slot(qs, view->rows[r].item) &&
                    qs->items[view->rows[r].item] == qs->items[i])
                {
                    dup = r;
                    break;
                }
        if (dup >= 0)
        {
            view->rows[dup].key_mask |= 1u << i;
            continue;
        }
        int idx = view->row_count++;
        view->rows[idx] = (struct cc_row){
            .qs = qs, .index = idx, .item = i, .key_mask = 1u << i,
        };
    }

    int sec_h = view->row_count == CC_MAX_ROWS ? (vp->height - 10) / 5
                                               : MIN(52, (vp->height - 12) / 4);
    int hero_h = view->row_count == CC_MAX_ROWS
                 ? vp->height - 4 * sec_h
                 : MIN(64, vp->height - 3 * sec_h);
    int ry = 0;
    for (int r = 0; r < view->row_count; ++r)
    {
        int rh = r == 0 ? hero_h : sec_h;
        ui_view_init(&view->rows[r].view, (struct ui_rect){0, ry, vp->width, rh},
                     &cc_row_ops, &view->rows[r]);
        ui_view_append(&view->background, &view->rows[r].view);
        ry += rh;
    }
}

static void quickscreen_layout(struct gui_quickscreen *qs, const struct viewport *vp)
{
    struct quickscreen_page *view = qs->view;
    static const struct ui_view_ops background_ops = {.paint = quickscreen_paint_background};
    static const struct ui_view_ops card_ops = {.paint = quickscreen_paint_card};
    static const uint8_t slots[QUICKSCREEN_ITEM_COUNT] = {1, 0, 2, 3};

    static const struct ui_view_ops cc_background_ops = {
        .paint = cc_paint_background};

    ui_view_init(&view->background, (struct ui_rect){vp->x, vp->y, vp->width, vp->height},
                 view->full_page ? &cc_background_ops : &background_ops, view);
    if (view->full_page)
    {
        cc_build_rows(qs, vp);
        ui_page_init(&view->page, &view->background);
        ui_nav_push(&view->nav, &view->page, UI_SHEET);
        return;
    }

    int margin = MIN(view->theme.metric[UI_METRIC_MODAL_EDGE_MARGIN],
                     vp->width / 8);
    int gap = MIN(MAX(4, margin * 2 / 3), vp->height / 8);
    int width = MAX(1, (vp->width - 2 * margin - gap) / 2);
    int height = MAX(1, MIN(view->theme.metric[UI_METRIC_ROW_HEIGHT] * 2 + margin,
                            (vp->height - gap) / 2));
    int x = (vp->width - (2 * width + gap)) / 2;
    int y = (vp->height - (2 * height + gap)) / 2;

    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
    {
        if (!qs->items[i])
            continue;
        struct quickscreen_card *card = &view->cards[i];
        card->qs = qs;
        card->item = i;
        ui_view_init(&card->view, (struct ui_rect){
                     x + (slots[i] % 2) * (width + gap),
                     y + (slots[i] / 2) * (height + gap), width, height},
                     &card_ops, card);
        ui_view_append(&view->background, &card->view);
    }

    ui_page_init(&view->page, &view->background);
    ui_nav_push(&view->nav, &view->page, UI_SHEET);
}

static void quickscreen_present(struct quickscreen_page *view, bool changed)
{
    if (view->full_page)
    {
        ui_sheet_paint_slide(&view->sheet, view->underneath, view->tile.fb_ptr,
                             changed);
        return;
    }
    ui_sheet_paint(&view->sheet, view->underneath, view->tile.fb_ptr,
                   (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT}, changed);
}

static void quickscreen_copy_tile(struct quickscreen_page *view, struct ui_rect area)
{
    if (area.width <= 0 || area.height <= 0)
        return;
    fb_data *dst = lcd_framebuffer_default.fb_ptr + area.y * LCD_WIDTH + area.x;
    const fb_data *src = view->tile.fb_ptr + area.y * LCD_WIDTH + area.x;
    for (int y = 0; y < area.height; y++)
    {
        memcpy(dst, src, area.width * sizeof(*src));
        dst += LCD_WIDTH;
        src += LCD_WIDTH;
    }
    ui_mark_dirty_rect(SCREEN_MAIN, area.x, area.y, area.width, area.height);
    ui_flush_dirty();
}

static void quickscreen_chrome(struct quickscreen_page *view)
{
    sb_settle(SCREEN_MAIN);
    if (view->sheet_handle > 0)
    {
        int height = view->background.value[UI_Y];
        memcpy(view->tile.fb_ptr, lcd_framebuffer_default.fb_ptr,
               LCD_WIDTH * height * sizeof(fb_data));
        if (view->sheet.active)
            view->sheet.light = view->full_page ? -1 : 0;
    }
}

static void quickscreen_invalidate_row(struct gui_quickscreen *qs, int row)
{
    struct quickscreen_page *view = qs->view;
    if (row < 0 || row >= view->row_count)
        return;
    const int *b = view->background.value;
    const int *p = view->rows[row].view.value;
    ui_page_invalidate(&view->page, (struct ui_rect){
        b[UI_X] + p[UI_X], b[UI_Y] + p[UI_Y], p[UI_WIDTH], p[UI_HEIGHT]});
}

static void quickscreen_invalidate_card(struct gui_quickscreen *qs,
                                        enum quickscreen_item item)
{
    struct quickscreen_page *view = qs->view;
    if (item >= QUICKSCREEN_ITEM_COUNT || !qs->items[item])
        return;
    const int *b = view->background.value;
    const int *p = view->cards[item].view.value;
    ui_page_invalidate(&view->page, (struct ui_rect){
        b[UI_X] + p[UI_X], b[UI_Y] + p[UI_Y], p[UI_WIDTH], p[UI_HEIGHT]});
}

static void quickscreen_flush(struct quickscreen_page *view)
{
    if (view->sheet_handle > 0)
    {
        struct ui_rect damage = ui_page_paint(&view->page);
        if (view->sheet.active)
            quickscreen_present(view, damage.width > 0 && damage.height > 0);
        else
            quickscreen_copy_tile(view, damage);
        return;
    }
    if (ui_flush_inhibited())
    {
        struct ui_rect damage = ui_page_paint(&view->page);
        if (damage.width > 0 && damage.height > 0)
            ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width, damage.height);
    }
    else
    {
        ui_nav_draw(&view->nav);
        ui_flush_dirty();
    }
}

static void quickscreen_update(struct gui_quickscreen *qs, enum quickscreen_item item)
{
    struct quickscreen_page *view = qs->view;
    struct ui_view *background = &view->background;
    album_palette_refresh();
    unsigned generation = album_palette_generation();

    if (generation != view->palette_generation || item == QUICKSCREEN_ITEM_COUNT)
    {
        album_palette_snapshot(&view->palette);
        view->palette_generation = generation;
        ui_page_invalidate(&view->page, (struct ui_rect){
            background->value[UI_X], background->value[UI_Y],
            background->value[UI_WIDTH], background->value[UI_HEIGHT]});
    }
    else if (view->full_page)
    {
        for (int r = 0; r < view->row_count; ++r)
        {
            enum quickscreen_item ri = view->rows[r].item;
            bool match = (view->rows[r].key_mask & (1u << item)) != 0;
            if (!match && ri < QUICKSCREEN_ITEM_COUNT && qs->items[ri] &&
                !cider_slot(qs, ri) && !cider_slot(qs, item) &&
                qs->items[ri] == qs->items[item])
                match = true;
            if (match)
                quickscreen_invalidate_row(qs, r);
        }
    }
    else
    {
        for (enum quickscreen_item i = QUICKSCREEN_TOP; i < QUICKSCREEN_ITEM_COUNT; ++i)
        {
            /* Repaint duplicate settings too. Remote shuffle and repeat
             * retain separate values even if their local items match. */
            if (!qs->items[i] || (i != item && (cider_slot(qs, i) || cider_slot(qs, item) ||
                                              qs->items[i] != qs->items[item])))
                continue;
            quickscreen_invalidate_card(qs, i);
        }
    }
    quickscreen_flush(view);
}

static void quickscreen_update_row(struct gui_quickscreen *qs, int row)
{
    quickscreen_invalidate_row(qs, row);
    quickscreen_flush(qs->view);
}

static void quickscreen_update_focus(struct gui_quickscreen *qs,
                                     int prev, int next)
{
    quickscreen_invalidate_row(qs, prev);
    quickscreen_invalidate_row(qs, next);
    quickscreen_flush(qs->view);
}

static bool quickscreen_wheel_transport(const struct gui_quickscreen *qs,
                                        int button)
{
    if (button != ACTION_WPS_PLAY && button != ACTION_WPS_SKIPPREV &&
        button != ACTION_WPS_SKIPNEXT)
        return false;
    if (qs->cider)
    {
        cider_wps_action(button);
        return true;
    }
    int status = audio_status();
    if (button == ACTION_WPS_PLAY)
    {
        if (status & AUDIO_STATUS_PLAY)
        {
            if (status & AUDIO_STATUS_PAUSE)
                audio_resume();
            else
                audio_pause();
        }
        else if (playlist_amount() > 0)
            playlist_resume_track(MAX(0, global_status.resume_index),
                                  global_status.resume_crc32,
                                  global_status.resume_elapsed,
                                  global_status.resume_offset);
    }
    else if (status & AUDIO_STATUS_PLAY)
    {
        if (button == ACTION_WPS_SKIPNEXT)
            audio_next();
        else
            audio_prev();
    }
    return true;
}

static void quickscreen_step_volume(struct gui_quickscreen *qs, int dir)
{
    if (qs->cider)
    {
        cider_control("volume-step", dir);
        if (qs->view->full_page)
            quickscreen_update_row(qs, 0);
        return;
    }
    adjust_volume(dir);
    if (qs->view->full_page)
        global_status.last_volume_change = 0;
    talk_settle();
    if (global_settings.talk_menu)
        talk_value_decimal(sound_val2phys(SOUND_VOLUME, global_status.volume),
                           UNIT_DB, sound_numdecimals(SOUND_VOLUME), false);
    if (qs->view->full_page)
        quickscreen_update_row(qs, 0);
    else if (qs->volume_item < QUICKSCREEN_ITEM_COUNT)
        quickscreen_update(qs, qs->volume_item);
}

static void talk_qs_option(struct gui_quickscreen *qs,
                           enum quickscreen_item item, bool enqueue);

static void quickscreen_wheel_step(struct gui_quickscreen *qs, int button)
{
    struct quickscreen_page *view = qs->view;
    if (button == ACTION_STD_OK)
    {
        if (view->row_count > 1)
        {
            int prev = qs->focus_row;
            qs->focus_row = (qs->focus_row + 1) % view->row_count;
            qs->focus_tick = current_tick;
            if (qs->focus_row == 0)
                cond_talk_ids(LANG_VOLUME);
            else
                talk_qs_option(qs, view->rows[qs->focus_row].item, true);
            quickscreen_update_focus(qs, prev, qs->focus_row);
        }
        return;
    }

    bool fwd = (button == ACTION_STD_NEXT || button == ACTION_STD_NEXTREPEAT);
    if (qs->focus_row == 0)
    {
        quickscreen_step_volume(qs, fwd ? 1 : -1);
        return;
    }

    qs->focus_tick = current_tick;
    enum quickscreen_item item = view->rows[qs->focus_row].item;
    if (item >= QUICKSCREEN_ITEM_COUNT || !qs->items[item])
        return;

    bool changed = false;
#ifdef HAVE_USB_PODLINK
    if (cider_slot(qs, item))
    {
        int mode = cider_slot_mode(item);
        cider_mode_set(mode, cider_mode_step(mode, cider_mode_get(mode), !fwd));
        changed = true;
    }
    else
#endif
    {
        const struct settings_list *setting = qs->items[item];
        int count = 0, position = 0;
        if ((setting->flags & F_T_MASK) == F_T_BOOL)
        {
            if (option_value_as_int(setting) != (fwd ? 1 : 0))
            {
                option_select_next_val(setting, false, true);
                changed = true;
            }
        }
        else if ((setting->flags & (F_INT_SETTING | F_T_SOUND)) &&
                 get_setting_info_for_bar(setting, 0, &count, &position) &&
                 count > 2)
        {
            if ((fwd && position < count - 1) || (!fwd && position > 0))
            {
                option_select_next_val(setting, !fwd, true);
                changed = true;
            }
        }
        else
        {
            option_select_next_val(setting, !fwd, true);
            changed = true;
        }
    }
    if (changed)
    {
        qs->result |= QUICKSCREEN_CHANGED;
        talk_qs_option(qs, item, false);
        quickscreen_update_row(qs, qs->focus_row);
    }
}

static void quickscreen_wheel_idle(struct gui_quickscreen *qs)
{
    if (qs->view->wheel && qs->focus_row != 0 &&
        TIME_AFTER(current_tick, qs->focus_tick + CC_FOCUS_TIMEOUT))
    {
        int prev = qs->focus_row;
        qs->focus_row = 0;
        quickscreen_update_focus(qs, prev, 0);
    }
}

static void quickscreen_draw_cb(unsigned short id, void *data, void *userdata)
{
    (void)id;
    (void)data;
    quickscreen_update(userdata, QUICKSCREEN_ITEM_COUNT);
}

/* The system handler runs USB in place. Release the view and restore the
 * caller's chrome before it takes control of the display. */
static void quickscreen_cleanup(void *param)
{
    struct gui_quickscreen *qs = param;
    struct quickscreen_page *view = qs->view;
    remove_event_ex(GUI_EVENT_NEED_UI_UPDATE, quickscreen_draw_cb, qs);

    ui_nav_pop(&view->nav);
    ui_sheet_cancel(&view->sheet);
    if (view->sheet_handle > 0)
    {
        core_put_data_pinned(view->underneath);
        core_free(view->sheet_handle);
    }
    sb_set_persistent_title(view->has_saved_title ? view->saved_title : NULL,
                            view->saved_icon, SCREEN_MAIN);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
    font_unload(view->title_font);
    font_unload(view->body_font);
    core_put_data_pinned(view);
    qs->view_handle = core_free(qs->view_handle);

    pop_current_activity();
}

static void talk_qs_option(struct gui_quickscreen *qs,
                           enum quickscreen_item item, bool enqueue)
{
    const struct settings_list *opt = qs->items[item];
    if (!global_settings.talk_menu || !opt || cider_slot(qs, item))
        return;

    if (enqueue)
        talk_id(opt->lang_id, enqueue);
    option_talk_value(opt, option_value_as_int(opt), enqueue);
}

/*
 * Does the actions associated to the given button if any
 *  - qs : the quickscreen
 *  - button : the key we are going to analyse
 * returns : true if the button corresponded to an action, false otherwise
 */
static bool gui_quickscreen_do_button(struct gui_quickscreen * qs, int button,
                                      enum quickscreen_item *item_out)
{
    int item;
    bool previous = false;
    switch(button)
    {
        case ACTION_QS_TOP:
            item = QUICKSCREEN_TOP;
            break;

        case ACTION_QS_LEFT:
            item = QUICKSCREEN_LEFT;
            previous = true;
            break;

        case ACTION_QS_DOWN:
            item = QUICKSCREEN_BOTTOM;
            previous = true;
            break;

        case ACTION_QS_RIGHT:
            item = QUICKSCREEN_RIGHT;
            break;

        default:
            return false;
    }

    if (qs->items[item] == NULL)
        return false;

#ifdef HAVE_USB_PODLINK
    if (cider_slot(qs, item))
    {
        int mode = cider_slot_mode(item);
        cider_mode_set(mode, cider_mode_step(mode, cider_mode_get(mode),
                                             previous));
    }
    else
#endif
        option_select_next_val(qs->items[item], previous, true);
    talk_qs_option(qs, item, false);
    *item_out = item;
    return true;
}


bool quick_screen_uses_hud(void)
{
    return ui_theme_style(UI_STYLE_QUICK_SCREEN) != UI_QUICK_SCREEN_TILES;
}

static int gui_syncquickscreen_run(struct gui_quickscreen * qs, int button_enter, bool *usb)
{
    int button;
    enum quickscreen_item changed;
    /* To quit we need either :
     *  - a second press on the button that made us enter
     *  - an action taken while pressing the enter button,
     *    then release the enter button*/
    bool can_quit = false;
    qs->result = QUICKSCREEN_OK;
    qs->focus_row = 0;
    qs->focus_tick = current_tick;
    *usb = false;
    qs->view_handle = core_alloc_free_space(sizeof(*qs->view));
    if (qs->view_handle <= 0)
        return QUICKSCREEN_OK;

    struct quickscreen_page *view = core_get_data_pinned(qs->view_handle);
    qs->view = view;
    *view = (struct quickscreen_page){ .paint_target = &lcd_framebuffer_default };
    view->full_page = quick_screen_uses_hud();
    view->wheel = ui_wheel_navigation();
    view->sheet.slide = view->full_page;
    ui_scene_interrupt();
    /* Keep the caller's frame so closing restores it without an extra repaint. */
    if ((global_settings.animations || view->full_page) && lcd_active())
        view->sheet_handle = core_alloc_free_space(2 * FRAMEBUFFER_SIZE);
    if (view->sheet_handle > 0)
    {
        view->underneath = core_get_data_pinned(view->sheet_handle);
        memcpy(view->underneath, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
        fb_data *body = view->underneath + QUICK_PIXELS;
        memcpy(body, view->underneath, FRAMEBUFFER_SIZE);
        view->tile = (struct frame_buffer_t){ .fb_ptr = body, .stride = LCD_WIDTH, .elems = QUICK_PIXELS };
        view->paint_target = &view->tile;
    }
    view->has_saved_title = sb_get_persistent_title(SCREEN_MAIN, view->saved_title,
                                                   sizeof(view->saved_title), &view->saved_icon);
    ui_theme_snapshot(&view->theme);
    view->title_font = ui_theme_font_load(&view->theme, UI_FONT_TITLE, 48);
    view->body_font = ui_theme_font_load(&view->theme,
                                         view->full_page ? UI_FONT_LIST
                                                         : UI_FONT_BODY, 64);

    push_current_activity(ACTIVITY_QUICKSCREEN);
    sb_set_persistent_title(str(LANG_QUICK_SETTINGS), Icon_NOICON, SCREEN_MAIN);
    screens[SCREEN_MAIN].scroll_stop();
    struct viewport content;
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    view->fallback_font = content.font;
    quickscreen_layout(qs, &content);
    quickscreen_chrome(view);
    if (view->sheet_handle > 0)
    {
        ui_sheet_begin(&view->sheet, true, "quick-settings");
        if (view->sheet.active)
        {
            int top = view->background.value[UI_Y];
            memcpy(lcd_framebuffer_default.fb_ptr, view->underneath, LCD_WIDTH * top * sizeof(fb_data));
            ui_take_dirty(SCREEN_MAIN);
            view->sheet.light = view->full_page ? -1 : UI_FULL_OPACITY;
        }
    }
    quickscreen_update(qs, QUICKSCREEN_ITEM_COUNT);
    add_event_ex(GUI_EVENT_NEED_UI_UPDATE, false, quickscreen_draw_cb, qs);
    /* Announce current selection on entering this screen. This is all
       queued up, but can be interrupted as soon as a setting is
       changed. */
    cond_talk_ids(VOICE_QUICKSCREEN);
    talk_qs_option(qs, QUICKSCREEN_TOP, true);
    if (qs->items[QUICKSCREEN_TOP] != qs->items[QUICKSCREEN_BOTTOM])
        talk_qs_option(qs, QUICKSCREEN_BOTTOM, true);
    talk_qs_option(qs, QUICKSCREEN_LEFT, true);
    if (qs->items[QUICKSCREEN_LEFT] != qs->items[QUICKSCREEN_RIGHT])
        talk_qs_option(qs, QUICKSCREEN_RIGHT, true);

    while (true) {
        if (qs->poll)
            qs->poll();
        /* Keep processing actions while the sheet is in flight. */
        bool was_inhibited = ui_flush_inhibited();
        if (view->sheet.active)
            ui_inhibit_flush(true);
        button = get_action(CONTEXT_QUICKSCREEN, ui_sheet_timeout(&view->sheet, HZ/5));
        ui_inhibit_flush(was_inhibited);
        /* The USB screen runs inside the handler, so the quickscreen has to
         * take itself down first -- otherwise it is still the current
         * activity, with its theme viewport in place, underneath it. */
        if (button == SYS_USB_CONNECTED)
        {
            default_event_handler_ex(button, quickscreen_cleanup, qs);
            *usb = true;
            return qs->result | QUICKSCREEN_IN_USB;
        }
        default_event_handler(button);
        if (view->wheel)
        {
            if (quickscreen_wheel_transport(qs, button))
                button = ACTION_NONE;
            else if (button == ACTION_STD_OK ||
                     button == ACTION_STD_NEXT || button == ACTION_STD_PREV ||
                     button == ACTION_STD_NEXTREPEAT ||
                     button == ACTION_STD_PREVREPEAT)
            {
                quickscreen_wheel_step(qs, button);
                button = ACTION_NONE;
            }
            else
                quickscreen_wheel_idle(qs);
        }
        if (gui_quickscreen_do_button(qs, button, &changed))
        {
            qs->result |= QUICKSCREEN_CHANGED;
            can_quit = true;
            quickscreen_update(qs, changed);
        }
        else if (button == button_enter)
            can_quit = true;
        else if (qs->cider && (button == ACTION_QS_VOLUP ||
                               button == ACTION_QS_VOLDOWN))
        {
            cider_control("volume-step", button == ACTION_QS_VOLUP ? 1 : -1);
            if (view->full_page)
                quickscreen_update_row(qs, 0);
        }
        else if (button == ACTION_QS_VOLUP || button == ACTION_QS_VOLDOWN)
        {
            adjust_volume(button == ACTION_QS_VOLUP ? 1 : -1);
            if (view->full_page)
                global_status.last_volume_change = 0;
            talk_settle();
            if (global_settings.talk_menu)
                talk_value_decimal(sound_val2phys(SOUND_VOLUME,
                                                  global_status.volume),
                                   UNIT_DB, sound_numdecimals(SOUND_VOLUME),
                                   false);

            if (view->full_page)
                quickscreen_update_row(qs, 0);
            else if (qs->volume_item < QUICKSCREEN_ITEM_COUNT)
                quickscreen_update(qs, qs->volume_item);
        }
        else if (button == ACTION_STD_CONTEXT && !qs->cider)
        {
            qs->result |= QUICKSCREEN_GOTO_SHORTCUTS_MENU;
            break;
        }
        if ((button == button_enter) && can_quit)
            break;

        if (button == ACTION_STD_CANCEL)
            break;

        if (view->sheet.active && ui_take_dirty(SCREEN_MAIN))
            quickscreen_chrome(view);
        if (button == ACTION_REDRAW || album_palette_needs_repaint())
        {
            quickscreen_chrome(view);
            quickscreen_update(qs, QUICKSCREEN_ITEM_COUNT);
        }
        if (view->sheet.active)
            quickscreen_present(view, false);
    }
    if (view->sheet_handle > 0)
    {
        bool takeover = IS_SYSEVENT(button);
#ifdef HAVE_WHEEL_WPS_CHORD
        takeover |= agents_nav_pending() != AGENTS_NAV_NONE;
#endif
        if (!takeover)
        {
            ui_sheet_begin(&view->sheet, false, "quick-settings");
            if (view->full_page)
                view->sheet.light = -1;
            do {
                quickscreen_present(view, false);
            } while (ui_sheet_wait(&view->sheet));
            if (view->sheet.active)
            {
                memcpy(lcd_framebuffer_default.fb_ptr, view->underneath, FRAMEBUFFER_SIZE);
                ui_mark_dirty(SCREEN_MAIN);
                ui_flush_dirty();
            }
            if (view->full_page && lcd_active() && !lcd_updates_held() &&
                !ui_flush_inhibited())
                qs->result |= QUICKSCREEN_RESTORED;
        }
    }
    /* Notify that we're exiting this screen */
    cond_talk_ids_fq(VOICE_OK);
    quickscreen_cleanup(qs);

    return qs->result;
}

static int quick_screen_run(int button_enter, bool cider, void (*poll)(void))
{
    struct gui_quickscreen qs;
    int before[QUICKSCREEN_ITEM_COUNT];
    bool usb = false;

    qs.cider = cider;
    qs.poll = poll;
    qs.volume_item = QUICKSCREEN_ITEM_COUNT;

    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
    {
        qs.items[i] = global_settings.qs_items[i];

        if (!is_setting_quickscreenable(qs.items[i]))
            qs.items[i] = NULL;
        else if (qs.items[i] && !cider_slot(&qs, i))
        {
            before[i] = option_value_as_int(qs.items[i]);
            if (qs.items[i]->lang_id == LANG_VOLUME)
                qs.volume_item = i;
        }
    }

    if (cider)
    {
        /* Only horizontal directions change Cider settings. */
        qs.items[QUICKSCREEN_LEFT] = find_setting(&global_settings.shuffle_mode);
        qs.items[QUICKSCREEN_RIGHT] = find_setting(&global_settings.repeat_mode);
    }

    int ret = gui_syncquickscreen_run(&qs, button_enter, &usb);
    if (ret & QUICKSCREEN_CHANGED)
    {
        bool local_changed = false;
        /* Save the native setting changes together on exit. */
        for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
            if (qs.items[i] && !cider_slot(&qs, i) &&
                option_value_as_int(qs.items[i]) != before[i])
            {
                local_changed = true;
            }
        if (local_changed)
            settings_save();
    }
    if (usb)
        return QUICKSCREEN_IN_USB;
    if (ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU)
        return QUICKSCREEN_GOTO_SHORTCUTS_MENU;
    if (ret & QUICKSCREEN_RESTORED)
        return ret & (QUICKSCREEN_RESTORED | QUICKSCREEN_CHANGED);
    return QUICKSCREEN_OK;
}

int quick_screen_quick(int button_enter)
{
    return quick_screen_run(button_enter, false, NULL);
}

/* Cider owns LEFT/RIGHT; the local TOP/BOTTOM settings remain editable. */
int quick_screen_cider(int button_enter)
{
    return quick_screen_run(button_enter, true, NULL);
}

int quick_screen_current(int button_enter, void (*poll)(void))
{
    return quick_screen_run(button_enter, cider_active(), poll);
}

/* stuff to make the quickscreen configurable */
bool is_setting_quickscreenable(const struct settings_list *setting)
{
    if (!setting)
        return true;

    /* to keep things simple, only settings which have a lang_id set are ok */
    if (setting->lang_id < 0 || (setting->flags & F_BANFROMQS))
        return false;

    switch (setting->flags & F_T_MASK)
    {
        case F_T_BOOL:
            return true;
        case F_T_INT:
        case F_T_UINT:
            return (setting->RESERVED != NULL);
        default:
            return false;
    }
}
