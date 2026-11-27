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
#include <string.h>
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
    struct ui_view panel;   /* The HUD's glass, behind the cards. */
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
    bool hud;               /* style.quick_screen=hud */
};

struct gui_quickscreen
{
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    struct quickscreen_page *view;
    int view_handle;
    int result;
    int volume_item;
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

/* The HUD: a pane of dark glass over the dimmed screen, each setting in an
 * etched well, drawn as an iPhone OS switch, slider or pop-up button. */
#define HUD_INSET 8         /* glass around the wells */
#define HUD_RADIUS 10
#define HUD_SHADOW 4
#define HUD_DROP 2
#define HUD_CELL_RADIUS 6
#define HUD_PAD 8           /* inside a well */
#define HUD_CONTROL 22      /* control height */
#define HUD_KNOB 18         /* slider knob */
#define HUD_TRACK 7         /* slider track */
#define HUD_ARROW 3
#define HUD_TEXT 0xffffff
#define HUD_TEXT_SHADOW 0x000000
#define HUD_VALUE 0xc4cad2

static const struct gloss_fill hud_glass = {
    0xf0767b82, 0xee3a3e44, 0xf2141618, 0xf4000000 };
static const struct gloss_fill hud_gloss = {
    0x70ffffff, 0x30ffffff, 0x10ffffff, 0x10ffffff };
static const struct gloss_fill hud_well = {
    0x80000000, 0x48000000, 0x28000000, 0x24ffffff };
static const struct gloss_fill hud_knob_fill = {
    0xffffffff, 0xfff6f6f6, 0xffc6c8cc, 0xff9a9ca0 };
static const struct gloss_fill hud_track_off = {
    0xff8a8a8a, 0xffd4d4d4, 0xfffafafa, 0xffffffff };
static const struct gloss_fill hud_button_fill = {
    0xff7e8287, 0xff5a5e63, 0xff2a2c2f, 0xff1c1d1f };

static struct gloss_surface hud_surface(const struct quickscreen_page *view,
                                        const struct ui_rect *clip)
{
    return (struct gloss_surface){
        .pixels = view->paint_target->fb_ptr, .stride = LCD_WIDTH,
        .clip_x = clip->x, .clip_y = clip->y,
        .clip_w = clip->width, .clip_h = clip->height };
}

/* The stock selection bar's blue, as the theme sets its four stops. */
static struct gloss_fill hud_blue(const struct quickscreen_page *view)
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

static struct ui_rect hud_intersect(struct ui_rect a, struct ui_rect b)
{
    int x = MAX(a.x, b.x), y = MAX(a.y, b.y);
    return (struct ui_rect){ x, y,
        MAX(0, MIN(a.x + a.width, b.x + b.width) - x),
        MAX(0, MIN(a.y + a.height, b.y + b.height) - y) };
}

/* `text` with its line's top-left at screen (x, y), over a one-pixel shadow
 * `dy` rows away, clipped to `clip`. */
static void hud_text(const struct quickscreen_page *view, struct ui_rect clip,
                     int font, int x, int y, const char *text, uint32_t rgb,
                     uint32_t shadow, int dy)
{
    if (clip.width <= 0 || clip.height <= 0)
        return;
    struct viewport vp = quickscreen_viewport(view, &clip, font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_RGBPACK(shadow >> 16, (shadow >> 8) & 255,
                                   shadow & 255));
    lcd_putsxy(x - vp.x, y - vp.y + dy, text);
    lcd_set_foreground(LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255));
    lcd_putsxy(x - vp.x, y - vp.y, text);
    lcd_set_viewport(previous);
}

static int hud_text_width(const char *text, int font)
{
    int w = 0, h = 0;
    font_getstringsize(text, &w, &h, font);
    return w;
}

static void hud_paint_background(const struct ui_view *node,
                                 const struct ui_view_frame *frame)
{
    const struct quickscreen_page *view = node->data;
    const struct ui_rect *c = &frame->clip;
    fb_data *fb = view->paint_target->fb_ptr;
    for (int y = c->y; y < c->y + c->height; y++)
    {
        fb_data *row = fb + y * LCD_WIDTH + c->x;
        if (view->underneath)
            ui_sheet_dim(row, view->underneath + y * LCD_WIDTH + c->x,
                         c->width);
        else
            for (int x = 0; x < c->width; x++)
                row[x] = LCD_RGBPACK(64, 64, 64);
    }
}

static void hud_paint_panel(const struct ui_view *node,
                            const struct ui_view_frame *frame)
{
    const struct quickscreen_page *view = node->data;
    const struct ui_rect *b = &frame->bounds;
    struct gloss_surface s = hud_surface(view, &frame->clip);
    gloss_shadow(&s, b->x, b->y, b->width, b->height, HUD_RADIUS,
                 HUD_SHADOW, HUD_DROP, 120);
    gloss_rect(&s, b->x, b->y, b->width, b->height, HUD_RADIUS, HUD_RADIUS,
               &hud_glass);
    /* The highlight band over the top of the glass, ending in a hard line
     * as the era's gloss does. */
    gloss_rect(&s, b->x + 1, b->y + 1, b->width - 2, b->height * 2 / 5,
               HUD_RADIUS - 1, 0, &hud_gloss);
    gloss_outline(&s, b->x, b->y, b->width, b->height, HUD_RADIUS,
                  0x58ffffff);
}

static void hud_knob(struct gloss_surface *s, int x, int y, int w, int h,
                     int r)
{
    gloss_shadow(s, x, y, w, h, r, 2, 1, 110);
    gloss_rect(s, x, y, w, h, r, r, &hud_knob_fill);
    gloss_outline(s, x, y, w, h, r, 0xff5e6066);
}

static void hud_slider(const struct quickscreen_page *view,
                       struct gloss_surface *s, struct ui_rect area,
                       int position, int count)
{
    int left = area.x + HUD_KNOB / 2;
    int right = area.x + area.width - HUD_KNOB / 2;
    int knob = left + (count > 1 ? (right - left) * position / (count - 1) : 0);
    int ty = area.y + (area.height - HUD_TRACK) / 2;
    struct gloss_fill blue = hud_blue(view);
    struct gloss_surface fill = *s;
    /* The filled part is blue up to the knob, the rest the light track. */
    fill.clip_w = MAX(0, MIN(s->clip_x + s->clip_w, knob) - s->clip_x);
    gloss_rect(&fill, left - 2, ty, right - left + 4, HUD_TRACK,
               HUD_TRACK / 2, HUD_TRACK / 2, &blue);
    fill = *s;
    fill.clip_x = MAX(s->clip_x, knob);
    fill.clip_w = MAX(0, s->clip_x + s->clip_w - fill.clip_x);
    gloss_rect(&fill, left - 2, ty, right - left + 4, HUD_TRACK,
               HUD_TRACK / 2, HUD_TRACK / 2, &hud_track_off);
    gloss_outline(s, left - 2, ty, right - left + 4, HUD_TRACK, HUD_TRACK / 2,
                  0xc0000000);
    int ky = area.y + (area.height - HUD_KNOB) / 2;
    hud_knob(s, knob - HUD_KNOB / 2, ky, HUD_KNOB, HUD_KNOB, HUD_KNOB / 2);
}

static void hud_switch(const struct quickscreen_page *view,
                       struct gloss_surface *s, struct ui_rect clip,
                       struct ui_rect area, bool on)
{
    int w = MIN(area.width, 72), h = area.height, r = 5;
    int knob = w * 2 / 5;
    struct gloss_fill blue = hud_blue(view);
    gloss_rect(s, area.x, area.y, w, h, r, r, on ? &blue : &hud_track_off);
    gloss_outline(s, area.x, area.y, w, h, r, 0xff3c3e42);
    const char *label = str(on ? LANG_ON : LANG_OFF);
    int font = view->title_font >= 0 ? view->title_font : view->fallback_font;
    int lx = on ? area.x : area.x + knob;
    int tw = hud_text_width(label, font);
    int ty = area.y + (h - font_get(font)->height) / 2;
    struct ui_rect text = hud_intersect(clip, (struct ui_rect){
        lx, area.y, w - knob, h });
    if (on)
        hud_text(view, text, font, lx + (w - knob - tw) / 2, ty, label,
                 0xffffff, 0x10306a, -1);
    else
        hud_text(view, text, font, lx + (w - knob - tw) / 2, ty, label,
                 0x7a7a7a, 0xffffff, 1);
    hud_knob(s, on ? area.x + w - knob : area.x, area.y, knob, h, r);
}

/* A Leopard HUD pop-up button: the value, and the up and down marks. */
static void hud_button(const struct quickscreen_page *view,
                       struct gloss_surface *s, struct ui_rect clip,
                       struct ui_rect area, const char *value)
{
    int r = 5;
    gloss_rect(s, area.x, area.y, area.width, area.height, r, r,
               &hud_button_fill);
    gloss_rect(s, area.x + 1, area.y + 1, area.width - 2, area.height / 2,
               r - 1, 0, &hud_gloss);
    gloss_outline(s, area.x, area.y, area.width, area.height, r, 0x78ffffff);

    int mark = area.x + area.width - 10, my = area.y + area.height / 2;
    struct viewport vp = quickscreen_viewport(view, &clip, view->fallback_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
    for (int i = 0; i < 3; i++)
    {
        lcd_hline(mark - i - vp.x, mark + i - vp.x, my - 4 + i - vp.y);
        lcd_hline(mark - i - vp.x, mark + i - vp.x, my + 4 - i - vp.y);
    }
    lcd_set_viewport(previous);

    int font = view->title_font >= 0 ? view->title_font : view->fallback_font;
    int room = area.width - 24;
    int tw = MIN(hud_text_width(value, font), room);
    struct ui_rect text = hud_intersect(clip, (struct ui_rect){
        area.x + 6, area.y, room + 2, area.height });
    hud_text(view, text, font, area.x + 7 + (room - tw) / 2,
             area.y + (area.height - font_get(font)->height) / 2, value,
             HUD_TEXT, HUD_TEXT_SHADOW, -1);
}

static void hud_paint_card(const struct ui_view *node,
                           const struct ui_view_frame *frame)
{
    struct quickscreen_card *card = node->data;
    struct gui_quickscreen *qs = card->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    struct gloss_surface s = hud_surface(view, &frame->clip);
    int title_font = view->title_font >= 0 ? view->title_font
                                           : view->fallback_font;
    int body_font = view->body_font >= 0 ? view->body_font
                                         : view->fallback_font;
    int line = font_get(title_font)->height;
    char buffer[MAX_PATH];

    gloss_rect(&s, box->x, box->y, box->width, box->height, HUD_CELL_RADIUS,
               HUD_CELL_RADIUS, &hud_well);

    int control_h = MIN(HUD_CONTROL, box->height - line - HUD_PAD);
    int gap = MAX(0, (box->height - line - control_h) / 3);
    int ty = box->y + gap;
    struct ui_rect inner = hud_intersect(frame->clip, (struct ui_rect){
        box->x + HUD_PAD, box->y, box->width - 2 * HUD_PAD, box->height });
    int arrow_x = box->x + box->width - HUD_PAD - HUD_ARROW;
    int arrow_y = ty + line / 2;

    /* The direction that steps this setting, and its name. */
    struct viewport vp = quickscreen_viewport(view, &frame->clip, title_font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_RGBPACK(0, 0, 0));
    quickscreen_paint_arrow(card->item, arrow_x - vp.x, arrow_y - 1 - vp.y,
                            HUD_ARROW);
    lcd_set_foreground(LCD_RGBPACK(255, 255, 255));
    quickscreen_paint_arrow(card->item, arrow_x - vp.x, arrow_y - vp.y,
                            HUD_ARROW);
    lcd_set_viewport(previous);

    struct ui_rect title = inner;
    title.width = MAX(0, MIN(title.x + title.width,
                             arrow_x - HUD_ARROW - 4) - title.x);
    hud_text(view, title, title_font, box->x + HUD_PAD, ty,
             quickscreen_title(qs, card->item), HUD_TEXT, HUD_TEXT_SHADOW, -1);

    struct ui_rect control = { box->x + HUD_PAD, ty + line + gap,
                               box->width - 2 * HUD_PAD, control_h };
    const struct settings_list *setting = qs->items[card->item];
    const char *value = quickscreen_value(qs, card->item, buffer,
                                          sizeof(buffer));
    int count = 0, position = 0;
    if (cider_slot(qs, card->item))
        hud_button(view, &s, frame->clip, control, value);
    else if ((setting->flags & F_T_MASK) == F_T_BOOL)
        hud_switch(view, &s, frame->clip, control,
                   option_value_as_int(setting));
    else if ((setting->flags & (F_INT_SETTING | F_T_SOUND)) &&
             get_setting_info_for_bar(setting, 0, &count, &position) &&
             count > 2)
    {
        /* A number beside its name, right-aligned short of the arrow. */
        int vw = hud_text_width(value, body_font);
        int vx = arrow_x - HUD_ARROW - 6 - vw;
        hud_text(view, title, body_font, vx, ty, value, HUD_VALUE,
                 HUD_TEXT_SHADOW, -1);
        hud_slider(view, &s, control, position, count);
    }
    else
        hud_button(view, &s, frame->clip, control, value);
}

static void quickscreen_layout(struct gui_quickscreen *qs, const struct viewport *vp)
{
    struct quickscreen_page *view = qs->view;
    static const struct ui_view_ops background_ops = {.paint = quickscreen_paint_background};
    static const struct ui_view_ops card_ops = {.paint = quickscreen_paint_card};
    static const uint8_t slots[QUICKSCREEN_ITEM_COUNT] = {1, 0, 2, 3};

    static const struct ui_view_ops hud_background_ops = {
        .paint = hud_paint_background};
    static const struct ui_view_ops hud_panel_ops = {.paint = hud_paint_panel};
    static const struct ui_view_ops hud_card_ops = {.paint = hud_paint_card};
    int inset = view->hud ? HUD_INSET : 0;

    int margin = MIN(view->theme.metric[UI_METRIC_MODAL_EDGE_MARGIN] + inset,
                     vp->width / 8);
    int gap = MIN(MAX(4, margin * 2 / 3), vp->height / 8);
    int width = MAX(1, (vp->width - 2 * margin - gap) / 2);
    int height = MAX(1, MIN(view->theme.metric[UI_METRIC_ROW_HEIGHT] * 2 + margin,
                            (vp->height - gap) / 2));
    int x = (vp->width - (2 * width + gap)) / 2;
    int y = (vp->height - (2 * height + gap)) / 2;

    ui_view_init(&view->background, (struct ui_rect){vp->x, vp->y, vp->width, vp->height},
                 view->hud ? &hud_background_ops : &background_ops, view);
    struct ui_view *parent = &view->background;
    if (view->hud)
    {
        /* The glass, and the cards placed within it. */
        ui_view_init(&view->panel, (struct ui_rect){x - inset, y - inset,
                     2 * width + gap + 2 * inset, 2 * height + gap + 2 * inset},
                     &hud_panel_ops, view);
        ui_view_append(&view->background, &view->panel);
        parent = &view->panel;
        x = y = inset;
    }
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
        ui_view_append(parent, &card->view);
    }

    ui_page_init(&view->page, &view->background);
    ui_nav_push(&view->nav, &view->page, UI_SHEET);
}

static void quickscreen_present(struct quickscreen_page *view, bool changed)
{
    if (view->hud)
    {
        /* The glass with its shadow, in screen coordinates. */
        const int *b = view->background.value, *p = view->panel.value;
        struct ui_rect panel = {
            b[UI_X] + p[UI_X] - HUD_SHADOW, b[UI_Y] + p[UI_Y] - HUD_SHADOW,
            p[UI_WIDTH] + 2 * HUD_SHADOW,
            p[UI_HEIGHT] + 2 * HUD_SHADOW + HUD_DROP };
        ui_sheet_paint_zoom(&view->sheet, view->underneath, view->tile.fb_ptr,
                            panel, b[UI_Y]);
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
            view->sheet.light = 0; /* Status rendering wrote to the canonical panel. */
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
            const int *o = view->panel.value;  /* zero without the HUD */
            ui_page_invalidate(&view->page, (struct ui_rect){
                background->value[UI_X] + o[UI_X] + p[UI_X],
                background->value[UI_Y] + o[UI_Y] + p[UI_Y],
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
        option_select_next_val(qs->items[item], previous, true);
    talk_qs_option(qs, item, false);
    *item_out = item;
    return true;
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
    view->hud = ui_theme_style(UI_STYLE_QUICK_SCREEN) == UI_QUICK_SCREEN_HUD;
    view->sheet.zoom = view->hud;
    ui_scene_interrupt();
    /* The HUD dims the screen it opens over, so it keeps a copy even when
     * it does not animate. */
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
    view->title_font = ui_theme_font_load(&view->theme,
                                          view->hud ? UI_FONT_LIST_VALUE
                                                    : UI_FONT_TITLE, 48);
    view->body_font = ui_theme_font_load(&view->theme, UI_FONT_BODY, 64);

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
            cider_control("volume-step", button == ACTION_QS_VOLUP ? 1 : -1);
        else if (button == ACTION_QS_VOLUP || button == ACTION_QS_VOLDOWN)
        {
            adjust_volume(button == ACTION_QS_VOLUP ? 1 : -1);
            talk_settle();
            if (global_settings.talk_menu)
                talk_value_decimal(sound_val2phys(SOUND_VOLUME,
                                                  global_status.volume),
                                   UNIT_DB, sound_numdecimals(SOUND_VOLUME),
                                   false);

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
            ui_sheet_begin(&view->sheet, false, "quick-settings");
            do {
                quickscreen_present(view, false);
            } while (ui_sheet_wait(&view->sheet));
            if (view->sheet.active)
            {
                memcpy(lcd_framebuffer_default.fb_ptr, view->underneath, FRAMEBUFFER_SIZE);
                ui_mark_dirty(SCREEN_MAIN);
                ui_flush_dirty();
            }
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
    return ret & QUICKSCREEN_GOTO_SHORTCUTS_MENU ? QUICKSCREEN_GOTO_SHORTCUTS_MENU :
                                                   QUICKSCREEN_OK;
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
