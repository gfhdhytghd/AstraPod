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
#include <stdlib.h>
#include <string.h>
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
#ifdef HAVE_WHEEL_WPS_CHORD
#include "input/agents_shortcut.h"
#endif

#define QUICK_PIXELS (LCD_WIDTH * LCD_HEIGHT)

struct gui_quickscreen;
struct quickscreen_card {
    struct ui_view view;
    struct gui_quickscreen *qs;
    enum quickscreen_item item;
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
    unsigned palette_generation;
    int title_font, body_font, fallback_font;
    char saved_title[MAX_PATH];
    enum themable_icons saved_icon;
    bool has_saved_title;
    bool hud;               /* style.quick_screen=hud: full-screen 4-tile page */
};

struct gui_quickscreen
{
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    struct quickscreen_page *view;
    int view_handle;
    int result;
    int volume_item;
    int before[QUICKSCREEN_ITEM_COUNT];
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

/* Full-screen four-tile skeuomorphic Control Centre (Classic, Fruit Company,
 * Fruit Company Ultra): four pearl-white glossy tiles over the stock
 * steel-blue backdrop, with a vertical brightness slider, Aqua choice pills,
 * iPhone OS toggle switches, and brushed-chrome directional key caps. */
#define CC_MARGIN_X 8
#define CC_MARGIN_Y 10
#define CC_GUTTER 8
#define CC_TILE_R 9
#define CC_TILE_SHADOW 4
#define CC_TILE_DROP 2
#define CC_PAD 10
#define CC_KEY_SIZE 22
#define CC_CTRL_Y 46
#define CC_CTRL_H 36
#define CC_CTRL_R 7
#define CC_PILL_CAP 24
#define CC_SWITCH_KNOB 50
#define CC_VSLIDER_W 24
#define CC_VSLIDER_R 8
#define CC_VKNOB_H 14
#define CC_VKNOB_R 6
#define CC_HSLIDER_TRACK 10
#define CC_HSLIDER_KNOB 18

#define CC_TITLE_RGB 0x161c24
#define CC_EMBOSS_RGB 0xffffff
#define CC_VALUE_ACTIVE_RGB 0x103868
#define CC_VALUE_OFF_RGB 0x3b444e
#define CC_KEY_ARROW_RGB 0x224c7d
#define CC_SUN_RGB 0x3b526a

static const struct gloss_fill cc_backdrop_fill = {
    0xff8597a4, 0xff7f919e, 0xff49545c, 0xff454f56 };
static const struct gloss_fill cc_tile_fill = {
    0xffffffff, 0xfff8fafc, 0xffdce3ec, 0xffc3cedb };
static const struct gloss_fill cc_tile_gloss = {
    0x52ffffff, 0x36ffffff, 0x0cffffff, 0x06ffffff };
static const struct gloss_fill cc_key_fill = {
    0xffffffff, 0xfff3f6fa, 0xffcfd7e1, 0xffb3bdc9 };
static const struct gloss_fill cc_key_gloss = {
    0x64ffffff, 0x48ffffff, 0x10ffffff, 0x08ffffff };
static const struct gloss_fill cc_pill_fill = {
    0xffffffff, 0xfffafcfd, 0xffd5dde7, 0xffb9c4d1 };
static const struct gloss_fill cc_pill_gloss = {
    0x68ffffff, 0x44ffffff, 0x10ffffff, 0x08ffffff };
static const struct gloss_fill cc_switch_off = {
    0xffa6b0bc, 0xffcdd4dd, 0xffeef2f6, 0xffffffff };
static const struct gloss_fill cc_switch_gloss = {
    0x5cffffff, 0x36ffffff, 0x0cffffff, 0x00ffffff };
static const struct gloss_fill cc_knob_fill = {
    0xffffffff, 0xfff6f8fa, 0xffcfd5dd, 0xff9ea6b0 };
static const struct gloss_fill cc_trough_fill = {
    0xff8a94a0, 0xffb8c2ce, 0xffdde3ea, 0xffedf1f6 };
static const struct gloss_fill cc_tube_gloss = {
    0x44ffffff, 0x30ffffff, 0x12ffffff, 0x08ffffff };

static unsigned cc_pack(uint32_t rgb)
{
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static struct gloss_surface cc_surface(const struct quickscreen_page *view,
                                       const struct ui_rect *clip)
{
    return (struct gloss_surface){
        .pixels = view->paint_target->fb_ptr, .stride = LCD_WIDTH,
        .clip_x = clip->x, .clip_y = clip->y,
        .clip_w = clip->width, .clip_h = clip->height };
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

static struct gloss_surface cc_clip_surface(const struct gloss_surface *s,
                                            int x, int y, int w, int h)
{
    struct ui_rect r = cc_intersect(
        (struct ui_rect){ s->clip_x, s->clip_y, s->clip_w, s->clip_h },
        (struct ui_rect){ x, y, w, h });
    return (struct gloss_surface){
        .pixels = s->pixels, .stride = s->stride,
        .clip_x = r.x, .clip_y = r.y, .clip_w = r.width, .clip_h = r.height };
}

/* `text` with its line's top-left at screen (x, y), over a one-pixel emboss
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

/* Draw the steel-blue backdrop and each tile's soft drop shadow so shadows
 * extend cleanly into gutters and never double-darken on single-tile updates. */
static void hud_paint_background(const struct ui_view *node,
                                 const struct ui_view_frame *frame)
{
    const struct quickscreen_page *view = node->data;
    const struct ui_rect *c = &frame->clip;
    struct gloss_surface s = cc_surface(view, c);
    if (view->theme.style[UI_STYLE_ART] == UI_ART_PLASTIC && stock_art_ready())
    {
        fb_data *fb = view->paint_target->fb_ptr;
        for (int y = c->y; y < c->y + c->height; y++)
            stock_art_row(STOCK_ART_BACKGROUND_LIGHTBLUE, c->x, y,
                          fb + y * LCD_WIDTH + c->x, c->width);
    }
    else
    {
        gloss_rect(&s, frame->bounds.x, frame->bounds.y,
                   frame->bounds.width, frame->bounds.height,
                   0, 0, &cc_backdrop_fill);
    }

    int bx = frame->bounds.x, by = frame->bounds.y;
    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
    {
        const struct quickscreen_card *card = &view->cards[i];
        if (!card->qs)
            continue;
        const int *p = card->view.value;
        gloss_shadow(&s, bx + p[UI_X], by + p[UI_Y], p[UI_WIDTH], p[UI_HEIGHT],
                     CC_TILE_R, CC_TILE_SHADOW, CC_TILE_DROP, 95);
    }
}

static void cc_paint_key_arrow(enum quickscreen_item item, int cx, int cy)
{
    for (int i = 0; i < 5; i++)
    {
        if (item == QUICKSCREEN_TOP)
            lcd_hline(cx - i, cx + i, cy - 2 + i);
        else if (item == QUICKSCREEN_BOTTOM)
            lcd_hline(cx - i, cx + i, cy + 2 - i);
        else if (item == QUICKSCREEN_LEFT)
            lcd_vline(cx - 2 + i, cy - i, cy + i);
        else
            lcd_vline(cx + 2 - i, cy - i, cy + i);
    }
}

static void cc_paint_key_cap(const struct quickscreen_page *view,
                             struct gloss_surface *s, struct ui_rect clip,
                             int x, int y, enum quickscreen_item item)
{
    const int size = CC_KEY_SIZE, r = 6;
    gloss_shadow(s, x, y, size, size, r, 2, 1, 60);
    gloss_rect(s, x, y, size, size, r, r, &cc_key_fill);
    gloss_rect(s, x + 1, y + 1, size - 2, size / 2 - 1, r - 1, 0,
               &cc_key_gloss);
    gloss_outline(s, x, y, size, size, r, 0xff818d9c);

    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    int cx = x + size / 2 - vp.x;
    int cy = y + size / 2 - vp.y;
    lcd_set_foreground(cc_pack(CC_EMBOSS_RGB));
    cc_paint_key_arrow(item, cx, cy + 1);
    lcd_set_foreground(cc_pack(CC_KEY_ARROW_RGB));
    cc_paint_key_arrow(item, cx, cy);
    lcd_set_viewport(previous);
}

static void cc_paint_sun_one(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx * dx + dy * dy <= 16)
                lcd_drawpixel(cx + dx, cy + dy);
    static const int8_t dirs[8][2] = {
        { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
        { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 },
    };
    for (int d = 0; d < 8; d++)
    {
        int dx = dirs[d][0], dy = dirs[d][1];
        bool diag = dx != 0 && dy != 0;
        int s0 = diag ? 5 : 6;
        int s1 = diag ? 7 : 8;
        for (int k = s0; k <= s1; k++)
            lcd_drawpixel(cx + dx * k, cy + dy * k);
    }
}

static void cc_paint_sun(const struct quickscreen_page *view,
                         struct ui_rect clip, int cx, int cy)
{
    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(cc_pack(CC_EMBOSS_RGB));
    cc_paint_sun_one(cx - vp.x, cy + 1 - vp.y);
    lcd_set_foreground(cc_pack(CC_SUN_RGB));
    cc_paint_sun_one(cx - vp.x, cy - vp.y);
    lcd_set_viewport(previous);
}

static void cc_knob(struct gloss_surface *s, int x, int y, int w, int h, int r)
{
    gloss_shadow(s, x, y, w, h, r, 2, 1, 95);
    gloss_rect(s, x, y, w, h, r, r, &cc_knob_fill);
    gloss_outline(s, x, y, w, h, r, 0xff59626d);
}

/* Tall vertical glossy brightness slider on the right edge of its tile. */
static void cc_vertical_brightness(const struct quickscreen_page *view,
                                   struct gloss_surface *s, struct ui_rect clip,
                                   const struct ui_rect *box,
                                   enum quickscreen_item item,
                                   const char *title, int position, int count)
{
    int title_font = view->title_font >= 0 ? view->title_font
                                           : view->fallback_font;
    int tx = box->x + box->width - CC_PAD - CC_VSLIDER_W;
    int ty = box->y + 9;
    int th = box->height - 18;
    int pct = count > 1 ? (position * 100 + (count - 1) / 2) / (count - 1)
                        : 100;

    struct ui_rect left_clip = cc_intersect(clip, (struct ui_rect){
        box->x + CC_PAD, box->y, tx - 4 - (box->x + CC_PAD), box->height });
    cc_text(view, left_clip, title_font, box->x + CC_PAD, box->y + 11, title,
            CC_TITLE_RGB, CC_EMBOSS_RGB, 1);

    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    cc_text(view, left_clip, title_font, box->x + CC_PAD + 2, box->y + 38,
            pct_buf, CC_VALUE_ACTIVE_RGB, CC_EMBOSS_RGB, 1);

    cc_paint_key_cap(view, s, clip, box->x + CC_PAD,
                     box->y + box->height - CC_PAD - CC_KEY_SIZE, item);
    cc_paint_sun(view, clip, box->x + 47,
                 box->y + box->height - CC_PAD - CC_KEY_SIZE / 2);

    /* Recessed glass tube filled bottom-to-top with stock glossy blue. */
    gloss_rect(s, tx, ty, CC_VSLIDER_W, th, CC_VSLIDER_R, CC_VSLIDER_R,
               &cc_trough_fill);
    int usable = MAX(1, th - CC_VKNOB_H);
    int knob_y = ty + usable -
                 (count > 1 ? usable * position / (count - 1) : usable);
    int fill_top = knob_y + CC_VKNOB_H / 2;
    struct gloss_fill blue = cc_blue(view);
    struct gloss_surface filled = cc_clip_surface(s, tx, fill_top,
                                                  CC_VSLIDER_W,
                                                  ty + th - fill_top);
    gloss_rect(&filled, tx, ty, CC_VSLIDER_W, th, CC_VSLIDER_R, CC_VSLIDER_R,
               &blue);
    gloss_rect(s, tx + 2, ty + 2, CC_VSLIDER_W / 2 - 2, th - 4, 4, 4,
               &cc_tube_gloss);

    /* Quartile stop ticks (25%, 50%, 75%) etched along the tube. */
    static const struct gloss_fill tick_dark = {
        0x38000000, 0x38000000, 0x38000000, 0x38000000 };
    static const struct gloss_fill tick_light = {
        0x48ffffff, 0x48ffffff, 0x48ffffff, 0x48ffffff };
    for (int k = 1; k <= 3; k++)
    {
        int tick_y = ty + CC_VKNOB_H / 2 + usable * k / 4;
        gloss_rect(s, tx + 4, tick_y, CC_VSLIDER_W - 8, 1, 0, 0, &tick_dark);
        gloss_rect(s, tx + 4, tick_y + 1, CC_VSLIDER_W - 8, 1, 0, 0,
                   &tick_light);
    }
    gloss_outline(s, tx, ty, CC_VSLIDER_W, th, CC_VSLIDER_R, 0xff5a6674);

    /* 3D brushed-chrome thumb riding over the tube. */
    int kw = CC_VSLIDER_W + 4;
    int kx = tx - 2;
    gloss_shadow(s, kx, knob_y, kw, CC_VKNOB_H, CC_VKNOB_R, 2, 1, 105);
    gloss_rect(s, kx, knob_y, kw, CC_VKNOB_H, CC_VKNOB_R, CC_VKNOB_R,
               &cc_knob_fill);
    gloss_outline(s, kx, knob_y, kw, CC_VKNOB_H, CC_VKNOB_R, 0xff4f5965);
    int ridge_y = knob_y + CC_VKNOB_H / 2;
    static const struct gloss_fill ridge_dark = {
        0x60404850, 0x60404850, 0x60404850, 0x60404850 };
    static const struct gloss_fill ridge_light = {
        0xa0ffffff, 0xa0ffffff, 0xa0ffffff, 0xa0ffffff };
    gloss_rect(s, kx + 7, ridge_y - 1, kw - 14, 1, 0, 0, &ridge_dark);
    gloss_rect(s, kx + 7, ridge_y, kw - 14, 1, 0, 0, &ridge_light);
}

/* iPhone OS-era glossy On/Off toggle switch. */
static void cc_switch(const struct quickscreen_page *view,
                      struct gloss_surface *s, struct ui_rect clip,
                      struct ui_rect area, bool on)
{
    int sw_w = area.width, sw_h = area.height, r = CC_CTRL_R;
    int knob_w = MIN(CC_SWITCH_KNOB, sw_w * 2 / 5);
    struct gloss_fill blue = cc_blue(view);

    gloss_shadow(s, area.x, area.y, sw_w, sw_h, r, 1, 1, 45);
    gloss_rect(s, area.x, area.y, sw_w, sw_h, r, r,
               on ? &blue : &cc_switch_off);
    if (on)
        gloss_rect(s, area.x + 1, area.y + 1, sw_w - 2, sw_h / 2 - 1,
                   r - 1, 0, &cc_switch_gloss);
    gloss_outline(s, area.x, area.y, sw_w, sw_h, r,
                  on ? 0xff1e4d80 : 0xff7b8592);

    const char *label = str(on ? LANG_ON : LANG_OFF);
    int font = view->body_font >= 0 ? view->body_font : view->fallback_font;
    int lx = on ? area.x : area.x + knob_w;
    int tw = cc_text_width(label, font);
    int ty = area.y + (sw_h - font_get(font)->height) / 2 - 1;
    struct ui_rect text = cc_intersect(clip, (struct ui_rect){
        lx, area.y, sw_w - knob_w, sw_h });
    if (on)
        cc_text(view, text, font, lx + (sw_w - knob_w - tw) / 2, ty, label,
                0xffffff, 0x002d64, -1);
    else
        cc_text(view, text, font, lx + (sw_w - knob_w - tw) / 2, ty, label,
                0x525c68, 0xffffff, 1);

    int kx = on ? area.x + sw_w - knob_w : area.x;
    cc_knob(s, kx, area.y, knob_w, sw_h, r);
    /* Three vertical tactile grip grooves on the chrome thumb. */
    int gx = kx + knob_w / 2;
    static const struct gloss_fill grip_dark = {
        0x50404850, 0x50404850, 0x50404850, 0x50404850 };
    static const struct gloss_fill grip_light = {
        0x80ffffff, 0x80ffffff, 0x80ffffff, 0x80ffffff };
    for (int dx = -4; dx <= 4; dx += 4)
    {
        gloss_rect(s, gx + dx, area.y + 10, 1, sw_h - 20, 0, 0, &grip_dark);
        gloss_rect(s, gx + dx + 1, area.y + 10, 1, sw_h - 20, 0, 0,
                   &grip_light);
    }
}

/* Aqua / Leopard pop-up choice pill with a glossy blue chevron cap. */
static void cc_choice_pill(const struct quickscreen_page *view,
                           struct gloss_surface *s, struct ui_rect clip,
                           struct ui_rect area, const char *value, bool active)
{
    int px = area.x, py = area.y, pw = area.width, ph = area.height;
    int r = CC_CTRL_R, cap_w = CC_PILL_CAP;
    struct gloss_fill blue = cc_blue(view);

    gloss_shadow(s, px, py, pw, ph, r, 2, 1, 55);
    gloss_rect(s, px, py, pw, ph, r, r, &cc_pill_fill);
    struct gloss_surface cap = cc_clip_surface(s, px + pw - cap_w, py,
                                               cap_w, ph);
    gloss_rect(&cap, px, py, pw, ph, r, r, &blue);
    gloss_rect(s, px + 1, py + 1, pw - 2, ph / 2 - 1, r - 1, 0,
               &cc_pill_gloss);

    static const struct gloss_fill sep = {
        0xff2b5f96, 0xff2b5f96, 0xff2b5f96, 0xff2b5f96 };
    gloss_rect(s, px + pw - cap_w, py + 1, 1, ph - 2, 0, 0, &sep);
    gloss_outline(s, px, py, pw, ph, r, 0xff6e7b8a);
    gloss_outline(&cap, px, py, pw, ph, r, 0xff1c4e82);

    /* Up/down arrows in the blue chevron cap. */
    int mx = px + pw - cap_w / 2, my = py + ph / 2;
    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(cc_pack(0x002d64));
    for (int i = 0; i < 4; i++)
    {
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my - 6 + i - vp.y);
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my + 4 - i - vp.y);
    }
    lcd_set_foreground(cc_pack(0xffffff));
    for (int i = 0; i < 4; i++)
    {
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my - 5 + i - vp.y);
        lcd_hline(mx - i - vp.x, mx + i - vp.x, my + 5 - i - vp.y);
    }
    lcd_set_viewport(previous);

    int font = view->body_font >= 0 ? view->body_font : view->fallback_font;
    int body_w = pw - cap_w;
    int room = body_w - 8;
    int tw = MIN(cc_text_width(value, font), room);
    int tx = px + MAX(4, (body_w - tw) / 2);
    int ty = py + (ph - font_get(font)->height) / 2 - 1;
    struct ui_rect text = cc_intersect(clip, (struct ui_rect){
        px + 4, py, room, ph });
    cc_text(view, text, font, tx, ty, value,
            active ? CC_VALUE_ACTIVE_RGB : CC_VALUE_OFF_RGB,
            CC_EMBOSS_RGB, 1);
}

/* Horizontal glossy Aqua slider for any non-brightness numeric setting. */
static void cc_horizontal_slider(const struct quickscreen_page *view,
                                 struct gloss_surface *s, struct ui_rect area,
                                 int position, int count)
{
    int left = area.x + CC_HSLIDER_KNOB / 2;
    int right = area.x + area.width - CC_HSLIDER_KNOB / 2;
    int knob = left + (count > 1 ? (right - left) * position / (count - 1) : 0);
    int ty = area.y + (area.height - CC_HSLIDER_TRACK) / 2;
    struct gloss_fill blue = cc_blue(view);

    struct gloss_surface fill = cc_clip_surface(s, area.x, ty,
                                                knob - area.x,
                                                CC_HSLIDER_TRACK);
    gloss_rect(&fill, area.x, ty, area.width, CC_HSLIDER_TRACK,
               CC_HSLIDER_TRACK / 2, CC_HSLIDER_TRACK / 2, &blue);
    fill = cc_clip_surface(s, knob, ty, area.x + area.width - knob,
                           CC_HSLIDER_TRACK);
    gloss_rect(&fill, area.x, ty, area.width, CC_HSLIDER_TRACK,
               CC_HSLIDER_TRACK / 2, CC_HSLIDER_TRACK / 2, &cc_trough_fill);
    gloss_outline(s, area.x, ty, area.width, CC_HSLIDER_TRACK,
                  CC_HSLIDER_TRACK / 2, 0xff6e7782);
    int ky = area.y + (area.height - CC_HSLIDER_KNOB) / 2;
    cc_knob(s, knob - CC_HSLIDER_KNOB / 2, ky,
            CC_HSLIDER_KNOB, CC_HSLIDER_KNOB, CC_HSLIDER_KNOB / 2);
}

static void hud_paint_card(const struct ui_view *node,
                           const struct ui_view_frame *frame)
{
    struct quickscreen_card *card = node->data;
    struct gui_quickscreen *qs = card->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    struct gloss_surface s = cc_surface(view, &frame->clip);
    int title_font = view->title_font >= 0 ? view->title_font
                                           : view->fallback_font;
    int body_font = view->body_font >= 0 ? view->body_font
                                         : view->fallback_font;
    char buffer[MAX_PATH];

    /* Pearl-white convex tile shell with upper-half glass gloss. */
    gloss_rect(&s, box->x, box->y, box->width, box->height,
               CC_TILE_R, CC_TILE_R, &cc_tile_fill);
    gloss_rect(&s, box->x + 1, box->y + 1, box->width - 2,
               box->height * 36 / 100, CC_TILE_R - 1, 0, &cc_tile_gloss);
    gloss_outline(&s, box->x, box->y, box->width, box->height,
                  CC_TILE_R, 0xff5a6878);

    const struct settings_list *setting = qs->items[card->item];
    const char *title = quickscreen_title(qs, card->item);
    const char *value = quickscreen_value(qs, card->item, buffer,
                                          sizeof(buffer));
    int count = 0, position = 0;

    if (!cider_slot(qs, card->item) && setting->lang_id == LANG_BRIGHTNESS &&
        get_setting_info_for_bar(setting, 0, &count, &position) && count > 1)
    {
        cc_vertical_brightness(view, &s, frame->clip, box, card->item,
                               title, position, count);
        return;
    }

    int key_x = box->x + box->width - CC_PAD - CC_KEY_SIZE;
    struct ui_rect title_clip = cc_intersect(frame->clip, (struct ui_rect){
        box->x + CC_PAD, box->y, key_x - 4 - (box->x + CC_PAD), box->height });
    cc_text(view, title_clip, title_font, box->x + CC_PAD, box->y + 11, title,
            CC_TITLE_RGB, CC_EMBOSS_RGB, 1);
    cc_paint_key_cap(view, &s, frame->clip, key_x, box->y + 9, card->item);

    struct ui_rect control = { box->x + CC_PAD, box->y + CC_CTRL_Y,
                               box->width - 2 * CC_PAD, CC_CTRL_H };
    if (cider_slot(qs, card->item))
    {
        bool active = cider_mode_get(cider_slot_mode(card->item)) != 0;
        cc_choice_pill(view, &s, frame->clip, control, value, active);
    }
    else if ((setting->flags & F_T_MASK) == F_T_BOOL)
    {
        cc_switch(view, &s, frame->clip, control,
                  option_value_as_int(setting) != 0);
    }
    else if ((setting->flags & (F_INT_SETTING | F_T_SOUND)) &&
             get_setting_info_for_bar(setting, 0, &count, &position) &&
             count > 2)
    {
        int vw = cc_text_width(value, body_font);
        int vx = box->x + box->width - CC_PAD - vw;
        struct ui_rect val_clip = cc_intersect(frame->clip, (struct ui_rect){
            box->x + CC_PAD, box->y + 34, box->width - 2 * CC_PAD, 24 });
        cc_text(view, val_clip, body_font, vx, box->y + 34, value,
                CC_VALUE_ACTIVE_RGB, CC_EMBOSS_RGB, 1);
        struct ui_rect bar = { box->x + CC_PAD, box->y + 62,
                               box->width - 2 * CC_PAD, 22 };
        cc_horizontal_slider(view, &s, bar, position, count);
    }
    else
    {
        bool active = option_value_as_int(setting) != 0;
        cc_choice_pill(view, &s, frame->clip, control, value, active);
    }
}

static void quickscreen_layout(struct gui_quickscreen *qs, const struct viewport *vp)
{
    struct quickscreen_page *view = qs->view;
    static const struct ui_view_ops background_ops = {.paint = quickscreen_paint_background};
    static const struct ui_view_ops card_ops = {.paint = quickscreen_paint_card};
    static const uint8_t slots[QUICKSCREEN_ITEM_COUNT] = {1, 0, 2, 3};

    static const struct ui_view_ops hud_background_ops = {
        .paint = hud_paint_background};
    static const struct ui_view_ops hud_card_ops = {.paint = hud_paint_card};

    int x, y, width, height, gap;
    if (view->hud)
    {
        gap = CC_GUTTER;
        width = MAX(1, (vp->width - 2 * CC_MARGIN_X - gap) / 2);
        height = MAX(1, (vp->height - 2 * CC_MARGIN_Y - gap) / 2);
        x = (vp->width - (2 * width + gap)) / 2;
        y = (vp->height - (2 * height + gap)) / 2;
    }
    else
    {
        int margin = MIN(view->theme.metric[UI_METRIC_MODAL_EDGE_MARGIN],
                         vp->width / 8);
        gap = MIN(MAX(4, margin * 2 / 3), vp->height / 8);
        width = MAX(1, (vp->width - 2 * margin - gap) / 2);
        height = MAX(1, MIN(view->theme.metric[UI_METRIC_ROW_HEIGHT] * 2 + margin,
                            (vp->height - gap) / 2));
        x = (vp->width - (2 * width + gap)) / 2;
        y = (vp->height - (2 * height + gap)) / 2;
    }

    ui_view_init(&view->background, (struct ui_rect){vp->x, vp->y, vp->width, vp->height},
                 view->hud ? &hud_background_ops : &background_ops, view);
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
                     view->hud ? &hud_card_ops : &card_ops, card);
        ui_view_append(&view->background, &card->view);
    }

    ui_page_init(&view->page, &view->background);
    ui_nav_push(&view->nav, &view->page, UI_SHEET);
}

static void quickscreen_present(struct quickscreen_page *view, bool changed)
{
    if (view->hud)
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
            view->sheet.light = -1; /* Status rendering wrote to the canonical panel. */
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
    else
    {
        for (enum quickscreen_item i = QUICKSCREEN_TOP; i < QUICKSCREEN_ITEM_COUNT; ++i)
        {
            /* Repaint duplicate settings too. Remote shuffle and repeat
             * retain separate values even if their local items match. */
            if (!qs->items[i] || (i != item && (cider_slot(qs, i) || cider_slot(qs, item) ||
                                              qs->items[i] != qs->items[item])))
                continue;
            const int *p = view->cards[i].view.value;
            ui_page_invalidate(&view->page, (struct ui_rect){
                background->value[UI_X] + p[UI_X],
                background->value[UI_Y] + p[UI_Y],
                p[UI_WIDTH], p[UI_HEIGHT]});
        }
    }
    if (view->sheet_handle > 0)
    {
        struct ui_rect damage = ui_page_paint(&view->page);
        if (view->sheet.active)
        {
            quickscreen_present(view, damage.width > 0 && damage.height > 0);
        }
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
    if (qs->view->hud && qs->items[item]->lang_id == LANG_BRIGHTNESS &&
        (qs->items[item]->flags & F_INT_SETTING) &&
        qs->items[item]->int_setting)
    {
        /* Single-tile vertical brightness slider: tap steps across five clean
         * stops and wraps max->min; hold ramps smoothly up and stops at max. */
        const struct int_setting *info = qs->items[item]->int_setting;
        int min_v = info->min, max_v = info->max;
        int cur = *(int *)qs->items[item]->setting;
        int next = min_v;
        if (get_action_statuscode(NULL) & ACTION_REPEAT)
        {
            int delta = (max_v - min_v) / 20;
            if (delta < 1)
                delta = 1;
            next = cur + delta;
            if (next > max_v)
                next = max_v;
        }
        else
        {
            for (int k = 1; k <= 4; k++)
            {
                int stop = min_v + (max_v - min_v) * k / 4;
                if (stop > cur)
                {
                    next = stop;
                    break;
                }
            }
        }
        *(int *)qs->items[item]->setting = next;
        if (info->option_callback)
            info->option_callback(next);
    }
    else
        option_select_next_val(qs->items[item], previous, true);
    talk_qs_option(qs, item, false);
    *item_out = item;
    return true;
}


bool quick_screen_uses_hud(void)
{
    return ui_theme_style(UI_STYLE_QUICK_SCREEN) == UI_QUICK_SCREEN_HUD;
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
    *usb = false;
    qs->view_handle = core_alloc_free_space(sizeof(*qs->view));
    if (qs->view_handle <= 0)
        return QUICKSCREEN_OK;

    struct quickscreen_page *view = core_get_data_pinned(qs->view_handle);
    qs->view = view;
    *view = (struct quickscreen_page){ .paint_target = &lcd_framebuffer_default };
    view->hud = quick_screen_uses_hud();
    /* Merge a legacy duplicate top/bottom brightness pair into the single
     * vertical brightness tile and fill the freed bottom slot with Enable EQ. */
    if (view->hud &&
        qs->items[QUICKSCREEN_TOP] && qs->items[QUICKSCREEN_BOTTOM] &&
        qs->items[QUICKSCREEN_TOP] == qs->items[QUICKSCREEN_BOTTOM] &&
        qs->items[QUICKSCREEN_TOP]->lang_id == LANG_BRIGHTNESS)
    {
        const struct settings_list *eq =
            find_setting(&global_settings.eq_enabled);
        if (eq)
        {
            qs->items[QUICKSCREEN_BOTTOM] = eq;
            qs->before[QUICKSCREEN_BOTTOM] = option_value_as_int(eq);
        }
    }
    view->sheet.slide = view->hud;
    ui_scene_interrupt();
    /* Keep the caller's framebuffer snapshot so closing restores it without a
     * caller redraw flash, even when animations are off. */
    if ((global_settings.animations || view->hud) && lcd_active())
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
                                         view->hud ? UI_FONT_LIST
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
            if (!view->sheet.slide)
                view->sheet.light = UI_FULL_OPACITY;
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
            quickscreen_chrome(view);
            if (!view->sheet.active)
                ui_flush_dirty();
        }
        else if (button == ACTION_QS_VOLUP || button == ACTION_QS_VOLDOWN)
        {
            adjust_volume(button == ACTION_QS_VOLUP ? 1 : -1);
            talk_settle();
            if (global_settings.talk_menu)
                talk_value_decimal(sound_val2phys(SOUND_VOLUME,
                                                  global_status.volume),
                                   UNIT_DB, sound_numdecimals(SOUND_VOLUME),
                                   false);

            quickscreen_chrome(view);
            if (!view->sheet.active)
                ui_flush_dirty();

            /* Volume changes repaint its card only when it is configured. */
            if (qs->volume_item < QUICKSCREEN_ITEM_COUNT)
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
            if (view->hud)
            {
                int top = view->background.value[UI_Y];
                memcpy(view->tile.fb_ptr, lcd_framebuffer_default.fb_ptr,
                       LCD_WIDTH * top * sizeof(fb_data));
            }
            ui_sheet_begin(&view->sheet, false, "quick-settings");
            bool first = true;
            do {
                quickscreen_present(view, first);
                first = false;
            } while (ui_sheet_wait(&view->sheet));
            if (view->sheet.active)
            {
                memcpy(lcd_framebuffer_default.fb_ptr, view->underneath, FRAMEBUFFER_SIZE);
                ui_mark_dirty(SCREEN_MAIN);
                ui_flush_dirty();
            }
            if (view->hud && lcd_active() && !lcd_updates_held() &&
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
            qs.before[i] = option_value_as_int(qs.items[i]);
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
                option_value_as_int(qs.items[i]) != qs.before[i])
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
