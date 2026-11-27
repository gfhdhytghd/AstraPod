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
    bool hud;               /* style.quick_screen=hud: the full-screen page */
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

/* TOP and BOTTOM name one integer setting with a range, so the page shows
 * it as one tall item with a bar. */
static bool qs_tall_range(const struct gui_quickscreen *qs)
{
    const struct settings_list *setting = qs->items[QUICKSCREEN_TOP];
    return setting && setting == qs->items[QUICKSCREEN_BOTTOM] &&
           (setting->flags & F_INT_SETTING) && setting->int_setting &&
           setting->int_setting->step > 0;
}

/* The full-screen page of Classic, Fruit Company and Fruit Company Ultra, in
 * the stock Settings preview's terms: its steel-blue backdrop, a glossy tile
 * on its reflection for each mode, white Helvetica Bold names over values,
 * and the Now Playing scrubber stood upright for a range. Nothing on it is a
 * button; the faint glyphs are the click wheel's legends. LEFT and RIGHT are
 * the left column's rows; TOP and BOTTOM share the right column, as one tall
 * item when they name one setting. */
#define CC_COLUMN 192           /* left column width; the right takes the rest */
#define CC_TILE 78              /* stock Settings tile, without its reflection */
#define CC_TILE_X 14
#define CC_TILE_Y 12
#define CC_REFLECTION 16        /* rows of the tile's reflection kept */
#define CC_TEXT_X 104           /* text beside a tile */
#define CC_TITLE_Y 26
#define CC_VALUE_Y 48
#define CC_KEY_Y 76
#define CC_TRACK_W 14           /* the stock scrubber's height */
#define CC_TRACK_TOP 64
#define CC_TRACK_BOTTOM 178
#define CC_KNOB 22

#define CC_WHITE 0xffffff
#define CC_VALUE_RGB 0xd6e4f2
#define CC_SHADOW_RGB 0x34424e
#define CC_KEY_ARGB 0xa8ffffff
#define CC_SUN_ARGB 0xebffffff

static const struct gloss_fill cc_backdrop_fill = {
    0xff98abbb, 0xff92a6b6, 0xff4d5760, 0xff49545a };
/* The round chrome knob of the stock sliders. */
static const struct gloss_fill cc_knob_fill = {
    0xffffffff, 0xfff8f9fb, 0xffc4c9d0, 0xff949aa4 };

static struct gloss_surface cc_surface(const struct quickscreen_page *view,
                                       const struct ui_rect *clip)
{
    return (struct gloss_surface){
        .pixels = view->paint_target->fb_ptr, .stride = LCD_WIDTH,
        .clip_x = clip->x, .clip_y = clip->y,
        .clip_w = clip->width, .clip_h = clip->height };
}

static void cc_dot(const struct gloss_surface *s, int x, int y, int w, int h,
                   uint32_t argb)
{
    const struct gloss_fill fill = { argb, argb, argb, argb };
    gloss_rect(s, x, y, w, h, 0, 0, &fill);
}

static int cc_font(const struct quickscreen_page *view, int font)
{
    return font >= 0 ? font : view->fallback_font;
}

/* `text` with its line's top-left at screen (x, y) over a shadow one row
 * lower, clipped to `clip`. */
static void cc_text(const struct quickscreen_page *view, struct ui_rect clip,
                    int font, int x, int y, const char *text, uint32_t rgb)
{
    if (clip.width <= 0 || clip.height <= 0)
        return;
    struct viewport vp = quickscreen_viewport(view, &clip, font);
    struct viewport *previous = lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_RGBPACK(CC_SHADOW_RGB >> 16,
                                   (CC_SHADOW_RGB >> 8) & 255,
                                   CC_SHADOW_RGB & 255));
    lcd_putsxy(x - vp.x, y - vp.y + 1, text);
    lcd_set_foreground(LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255));
    lcd_putsxy(x - vp.x, y - vp.y, text);
    lcd_set_viewport(previous);
}

static struct ui_rect cc_intersect(struct ui_rect a, struct ui_rect b)
{
    int x = MAX(a.x, b.x), y = MAX(a.y, b.y);
    return (struct ui_rect){ x, y,
        MAX(0, MIN(a.x + a.width, b.x + b.width) - x),
        MAX(0, MIN(a.y + a.height, b.y + b.height) - y) };
}

/* A triangle one column at a time: heights 1, 3, 5, 7 from its point. */
static void cc_triangle(const struct gloss_surface *s, int x, int y,
                        bool right)
{
    for (int k = 0; k < 4; k++)
        cc_dot(s, right ? x + 3 - k : x + k, y + 3 - k, 1, 2 * k + 1,
               CC_KEY_ARGB);
}

/* The click wheel's printed legend for `item`, its top-left at (x, y). */
static void cc_key(const struct gloss_surface *s, enum quickscreen_item item,
                   int x, int y)
{
    /* MENU in five-row capitals. */
    static const uint8_t menu[4][5] = {
        { 0x11, 0x1b, 0x15, 0x11, 0x11 }, { 0x0f, 0x08, 0x0e, 0x08, 0x0f },
        { 0x11, 0x19, 0x15, 0x13, 0x11 }, { 0x11, 0x11, 0x11, 0x11, 0x0e },
    };
    switch (item)
    {
        case QUICKSCREEN_TOP:
            for (int c = 0, gx = x; c < 4; c++)
            {
                int width = c == 1 ? 4 : 5;
                for (int row = 0; row < 5; row++)
                    for (int col = 0; col < width; col++)
                        if (menu[c][row] & (1 << (width - 1 - col)))
                            cc_dot(s, gx + col, y + 1 + row, 1, 1, CC_KEY_ARGB);
                gx += width + 1;
            }
            break;
        case QUICKSCREEN_LEFT:
            cc_dot(s, x, y, 2, 7, CC_KEY_ARGB);
            cc_triangle(s, x + 2, y, false);
            cc_triangle(s, x + 6, y, false);
            break;
        case QUICKSCREEN_RIGHT:
            cc_triangle(s, x, y, true);
            cc_triangle(s, x + 4, y, true);
            cc_dot(s, x + 8, y, 2, 7, CC_KEY_ARGB);
            break;
        default:
            cc_triangle(s, x, y, true);
            cc_dot(s, x + 6, y, 2, 7, CC_KEY_ARGB);
            cc_dot(s, x + 9, y, 2, 7, CC_KEY_ARGB);
            break;
    }
}

/* A sun centred on the pixel corner (cx, cy): a disc of radius `r` and
 * eight rays two pixels wide from `r0` to `r1` out. */
static void cc_sun(const struct gloss_surface *s, int cx, int cy, int r,
                   int r0, int r1, uint32_t argb)
{
    const struct gloss_fill fill = { argb, argb, argb, argb };
    int len = r1 - r0;
    gloss_rect(s, cx - r, cy - r, 2 * r, 2 * r, r, r, &fill);
    cc_dot(s, cx - 1, cy - r1, 2, len, argb);
    cc_dot(s, cx - 1, cy + r0, 2, len, argb);
    cc_dot(s, cx - r1, cy - 1, len, 2, argb);
    cc_dot(s, cx + r0, cy - 1, len, 2, argb);
    /* The diagonals step a pixel at a time; 181 / 256 is cos 45. */
    for (int t = r0 * 181 / 256 + 1; t <= r1 * 181 / 256; t++)
    {
        cc_dot(s, cx + t - 1, cy - t - 1, 2, 2, argb);
        cc_dot(s, cx - t - 1, cy - t - 1, 2, 2, argb);
        cc_dot(s, cx + t - 1, cy + t - 1, 2, 2, argb);
        cc_dot(s, cx - t - 1, cy + t - 1, 2, 2, argb);
    }
}

static int cc_brightness(const struct settings_list *setting)
{
    return setting && setting->setting == &global_settings.brightness;
}

/* The stock Settings tile that shows `item`'s value, or -1. */
static int cc_icon(struct gui_quickscreen *qs, enum quickscreen_item item)
{
    const struct settings_list *setting = qs->items[item];
    bool cider = cider_slot(qs, item);
    int mode = cider ? cider_slot_mode(item) : -1;
    if (mode == CIDER_MODE_SHUFFLE ||
        (!cider && setting->setting == &global_settings.shuffle_mode))
    {
        int value = cider ? cider_mode_get(mode) : option_value_as_int(setting);
        return value ? STOCK_ART_SETTINGS_SHUFFLESONGS
                     : STOCK_ART_SETTINGS_SHUFFLEOFF;
    }
    if (mode == CIDER_MODE_REPEAT ||
        (!cider && setting->setting == &global_settings.repeat_mode))
    {
        int value = cider ? cider_mode_get(mode) : option_value_as_int(setting);
        return value == REPEAT_OFF ? STOCK_ART_SETTINGS_REPEATOFF :
               value == REPEAT_ONE ? STOCK_ART_SETTINGS_REPEATONE :
                                     STOCK_ART_SETTINGS_REPEATALL;
    }
    if (cc_brightness(setting))
        return STOCK_ART_SETTINGS_BRIGHTNESS;
    return -1;
}

static fb_data cc_mix(fb_data d, fb_data s, int a)
{
    int r = FB_UNPACK_RED(d), g = FB_UNPACK_GREEN(d), b = FB_UNPACK_BLUE(d);
    r += (FB_UNPACK_RED(s) - r) * a / 256;
    g += (FB_UNPACK_GREEN(s) - g) * a / 256;
    b += (FB_UNPACK_BLUE(s) - b) * a / 256;
    return FB_RGBPACK(r, g, b);
}

/* The tile `icon` at (x, y) with the top of its reflection, which fades out
 * over CC_REFLECTION rows instead of the stock 43. */
static void cc_tile(const struct quickscreen_page *view, struct ui_rect clip,
                    int icon, int x, int y)
{
    fb_data *fb = view->paint_target->fb_ptr;
    int x0 = MAX(x, clip.x), x1 = MIN(x + CC_TILE, clip.x + clip.width);
    if (x1 <= x0)
        return;
    for (int row = 0; row < CC_TILE + CC_REFLECTION; row++)
    {
        int sy = y + row;
        if (sy < clip.y || sy >= clip.y + clip.height)
            continue;
        fb_data *out = fb + sy * LCD_WIDTH + x0;
        fb_data under[CC_TILE];
        memcpy(under, out, (x1 - x0) * sizeof(*out));
        stock_art_row(STOCK_ART_SETTINGS_BASEPANEL, x0 - x, row, out, x1 - x0);
        stock_art_mask_row(icon, x0 - x, row, out, x1 - x0, 0xeeeeee);
        if (row < CC_TILE)
            continue;
        int fade = (row - CC_TILE + 1) * 256 / (CC_REFLECTION + 1);
        for (int i = 0; i < x1 - x0; i++)
            out[i] = cc_mix(out[i], under[i], fade);
    }
}

/* The stock scrubber's column `c` of CC_TRACK_W, filled or not. */
static fb_data cc_track_color(const struct quickscreen_page *view, int c,
                              bool filled)
{
    fb_data pixel = 0;
    if (stock_art_ready())
    {
        stock_art_row(filled ? STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_FILL :
                      STOCK_ART_NOWPLAYING_PROGRESSBAR_ACTIVE_BACKGROUND,
                      8, c, &pixel, 1);
        return pixel;
    }
    uint32_t rgb = ui_theme_gradient_at(&view->theme.gradient[filled ?
        UI_GRADIENT_PROGRESS : UI_GRADIENT_PROGRESS_TRACK], c, CC_TRACK_W,
        filled ? 0x3185d0 : 0xdbdbdb);
    return FB_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

/* The scrubber on its side from `top` to `bottom`, filled from the bottom
 * up to the knob at `position` of `count`. */
static void cc_vertical_bar(const struct quickscreen_page *view,
                            const struct gloss_surface *s, int x, int top,
                            int bottom, int position, int count)
{
    /* Rows each column loses to the round ends. */
    static const uint8_t inset[CC_TRACK_W] = {
        4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4 };
    int usable = bottom - top - CC_KNOB;
    int knob = bottom - CC_KNOB / 2 -
               (count > 1 ? usable * position / (count - 1) : usable);
    for (int c = 0; c < CC_TRACK_W; c++)
    {
        fb_data track = cc_track_color(view, c, false);
        fb_data fill = cc_track_color(view, c, true);
        uint32_t argb[2] = {
            0xff000000 | FB_UNPACK_RED(track) << 16 |
                FB_UNPACK_GREEN(track) << 8 | FB_UNPACK_BLUE(track),
            0xff000000 | FB_UNPACK_RED(fill) << 16 |
                FB_UNPACK_GREEN(fill) << 8 | FB_UNPACK_BLUE(fill),
        };
        int y0 = top + inset[c], y1 = bottom - inset[c];
        cc_dot(s, x + c, y0, 1, MAX(0, knob - y0), argb[0]);
        cc_dot(s, x + c, MAX(y0, knob), 1, y1 - MAX(y0, knob), argb[1]);
    }
    gloss_outline(s, x - 1, top - 1, CC_TRACK_W + 2, bottom - top + 2,
                  (CC_TRACK_W + 2) / 2, 0xff3c4854);

    int kx = x + CC_TRACK_W / 2 - CC_KNOB / 2, ky = knob - CC_KNOB / 2;
    gloss_shadow(s, kx, ky, CC_KNOB, CC_KNOB, CC_KNOB / 2, 2, 1, 100);
    gloss_rect(s, kx, ky, CC_KNOB, CC_KNOB, CC_KNOB / 2, CC_KNOB / 2,
               &cc_knob_fill);
    gloss_outline(s, kx, ky, CC_KNOB, CC_KNOB, CC_KNOB / 2, 0xff5a616b);
}

static void hud_paint_background(const struct ui_view *node,
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
    gloss_rect(&s, frame->bounds.x, frame->bounds.y, frame->bounds.width,
               frame->bounds.height, 0, 0, &cc_backdrop_fill);
}

/* An item of the right column: its name, then a vertical bar for a
 * setting with a range, or its value. */
static void hud_paint_column(struct quickscreen_card *card,
                             const struct ui_view_frame *frame,
                             struct gloss_surface *s)
{
    struct gui_quickscreen *qs = card->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    const struct settings_list *setting = qs->items[card->item];
    int title_font = cc_font(view, view->title_font);
    int body_font = cc_font(view, view->body_font);
    int cx = box->x + box->width / 2;
    char buffer[MAX_PATH];
    const char *title = quickscreen_title(qs, card->item);
    int w = 0, h = 0;

    font_getstringsize(title, &w, &h, title_font);
    struct ui_rect text = cc_intersect(frame->clip, (struct ui_rect){
        box->x + 4, box->y, box->width - 8, box->height });
    cc_text(view, text, title_font, MAX(text.x, cx - w / 2), box->y + 12,
            title, CC_WHITE);

    int count = 0, position = 0;
    bool tall = qs->items[QUICKSCREEN_TOP] == qs->items[QUICKSCREEN_BOTTOM];
    if (qs_tall_range(qs) &&
        get_setting_info_for_bar(setting, 0, &count, &position) && count > 2)
    {
        if (cc_brightness(setting))
        {
            cc_sun(s, cx, box->y + 46, 5, 7, 11, CC_SUN_ARGB);
            cc_sun(s, cx, box->y + 194, 3, 5, 8, 0xc8ffffff);
        }
        cc_vertical_bar(view, s, cx - CC_TRACK_W / 2, box->y + CC_TRACK_TOP,
                        box->y + CC_TRACK_BOTTOM, position, count);
        cc_key(s, QUICKSCREEN_TOP, cx + 18, box->y + 43);
        cc_key(s, QUICKSCREEN_BOTTOM, cx + 16, box->y + 191);
        return;
    }

    const char *value = quickscreen_value(qs, card->item, buffer,
                                          sizeof(buffer));
    font_getstringsize(value, &w, &h, body_font);
    cc_text(view, text, body_font, MAX(text.x, cx - w / 2), box->y + 34,
            value, CC_VALUE_RGB);
    if (tall)
    {
        cc_key(s, QUICKSCREEN_TOP, cx - 11, box->y + 64);
        cc_key(s, QUICKSCREEN_BOTTOM, cx - 6, box->y + 80);
    }
    else
        cc_key(s, card->item, cx - (card->item == QUICKSCREEN_TOP ? 11 : 6),
               box->y + 62);
}

static void hud_paint_card(const struct ui_view *node,
                           const struct ui_view_frame *frame)
{
    struct quickscreen_card *card = node->data;
    struct gui_quickscreen *qs = card->qs;
    struct quickscreen_page *view = qs->view;
    const struct ui_rect *box = &frame->bounds;
    struct gloss_surface s = cc_surface(view, &frame->clip);
    char buffer[MAX_PATH];

    if (card->item == QUICKSCREEN_TOP || card->item == QUICKSCREEN_BOTTOM)
    {
        hud_paint_column(card, frame, &s);
        return;
    }

    int icon = stock_art_ready() ? cc_icon(qs, card->item) : -1;
    int tx = box->x + (icon >= 0 ? CC_TEXT_X : CC_TILE_X);
    if (icon >= 0)
        cc_tile(view, frame->clip, icon, box->x + CC_TILE_X,
                box->y + CC_TILE_Y);
    struct ui_rect text = cc_intersect(frame->clip, (struct ui_rect){
        tx, box->y, box->x + box->width - 6 - tx, box->height });
    cc_text(view, text, cc_font(view, view->title_font), tx,
            box->y + CC_TITLE_Y, quickscreen_title(qs, card->item), CC_WHITE);
    cc_text(view, text, cc_font(view, view->body_font), tx,
            box->y + CC_VALUE_Y,
            quickscreen_value(qs, card->item, buffer, sizeof(buffer)),
            CC_VALUE_RGB);
    cc_key(&s, card->item, tx, box->y + CC_KEY_Y);
}

/* The page's slot for `item`, or an empty rectangle for BOTTOM when it
 * repeats TOP and the right column is one tall item. */
static struct ui_rect hud_slot(const struct gui_quickscreen *qs,
                               enum quickscreen_item item,
                               const struct viewport *vp)
{
    int half = vp->height / 2;
    int right = vp->width - CC_COLUMN;
    bool tall = qs->items[QUICKSCREEN_TOP] == qs->items[QUICKSCREEN_BOTTOM];
    switch (item)
    {
        case QUICKSCREEN_LEFT:
            return (struct ui_rect){ 0, 0, CC_COLUMN, half };
        case QUICKSCREEN_RIGHT:
            return (struct ui_rect){ 0, half, CC_COLUMN, vp->height - half };
        case QUICKSCREEN_TOP:
            return (struct ui_rect){ CC_COLUMN, 0, right,
                                     tall ? vp->height : half };
        default:
            return tall ? (struct ui_rect){ 0 } :
                (struct ui_rect){ CC_COLUMN, half, right, vp->height - half };
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

    int margin = MIN(view->theme.metric[UI_METRIC_MODAL_EDGE_MARGIN],
                     vp->width / 8);
    int gap = MIN(MAX(4, margin * 2 / 3), vp->height / 8);
    int width = MAX(1, (vp->width - 2 * margin - gap) / 2);
    int height = MAX(1, MIN(view->theme.metric[UI_METRIC_ROW_HEIGHT] * 2 + margin,
                            (vp->height - gap) / 2));
    int x = (vp->width - (2 * width + gap)) / 2;
    int y = (vp->height - (2 * height + gap)) / 2;

    ui_view_init(&view->background, (struct ui_rect){vp->x, vp->y, vp->width, vp->height},
                 view->hud ? &hud_background_ops : &background_ops, view);
    for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
    {
        struct ui_rect slot = view->hud ? hud_slot(qs, i, vp) :
            (struct ui_rect){ x + (slots[i] % 2) * (width + gap),
                              y + (slots[i] / 2) * (height + gap),
                              width, height };
        if (!qs->items[i] || !slot.width)
            continue;
        struct quickscreen_card *card = &view->cards[i];
        card->qs = qs;
        card->item = i;
        ui_view_init(&card->view, slot,
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
    if (qs->view->hud && qs_tall_range(qs) &&
        (item == QUICKSCREEN_TOP || item == QUICKSCREEN_BOTTOM))
    {
        /* MENU raises the tall item and PLAY lowers it, a sixteenth of its
         * range a step, stopping at the ends. */
        const struct int_setting *info = qs->items[item]->int_setting;
        int *value = qs->items[item]->setting;
        int step = MAX(info->step, (info->max - info->min + 15) / 16);
        int next = *value + (previous ? -step : step);
        *value = MIN(MAX(next, info->min), info->max);
        if (info->option_callback)
            info->option_callback(*value);
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
    /* MENU raises Brightness here; the hold that opened the page must not. */
    if (view->hud)
        action_wait_for_release_preserve_queue();
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
