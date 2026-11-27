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
};

struct gui_quickscreen
{
    const struct settings_list *items[QUICKSCREEN_ITEM_COUNT];
    struct quickscreen_page *view;
    int view_handle;
    int result;
    int volume_item;
    enum quickscreen_item focus;
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
    lcd_set_foreground(quickscreen_color(view,
        card->item == card->qs->focus ? UI_COLOR_ACCENT : UI_COLOR_SEPARATOR));
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

static void quickscreen_layout(struct gui_quickscreen *qs, const struct viewport *vp)
{
    struct quickscreen_page *view = qs->view;
    static const struct ui_view_ops background_ops = {.paint = quickscreen_paint_background};
    static const struct ui_view_ops card_ops = {.paint = quickscreen_paint_card};
    static const uint8_t slots[QUICKSCREEN_ITEM_COUNT] = {1, 0, 2, 3};

    int margin = MIN(view->theme.metric[UI_METRIC_MODAL_EDGE_MARGIN], vp->width / 8);
    int gap = MIN(MAX(4, margin * 2 / 3), vp->height / 8);
    int width = MAX(1, (vp->width - 2 * margin - gap) / 2);
    int height = MAX(1, MIN(view->theme.metric[UI_METRIC_ROW_HEIGHT] * 2 + margin,
                            (vp->height - gap) / 2));
    int x = (vp->width - (2 * width + gap)) / 2;
    int y = (vp->height - (2 * height + gap)) / 2;

    ui_view_init(&view->background, (struct ui_rect){vp->x, vp->y, vp->width, vp->height},
                 &background_ops, view);
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
            ui_page_invalidate(&view->page, (struct ui_rect){
                background->value[UI_X] + p[UI_X], background->value[UI_Y] + p[UI_Y],
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
    qs->focus = QUICKSCREEN_ITEM_COUNT;
    if (ui_wheel_navigation())
        for (int i = 0; i < QUICKSCREEN_ITEM_COUNT; ++i)
            if (qs->items[i])
            {
                qs->focus = i;
                break;
            }
    *usb = false;
    qs->view_handle = core_alloc_free_space(sizeof(*qs->view));
    if (qs->view_handle <= 0)
        return QUICKSCREEN_OK;

    struct quickscreen_page *view = core_get_data_pinned(qs->view_handle);
    qs->view = view;
    *view = (struct quickscreen_page){ .paint_target = &lcd_framebuffer_default };
    ui_scene_interrupt();
    if (global_settings.animations && lcd_active())
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
        if (ui_wheel_navigation())
        {
            static const int slot_actions[] = {
                [QUICKSCREEN_TOP] = ACTION_QS_TOP,
                [QUICKSCREEN_BOTTOM] = ACTION_QS_DOWN,
                [QUICKSCREEN_LEFT] = ACTION_QS_LEFT,
                [QUICKSCREEN_RIGHT] = ACTION_QS_RIGHT,
            };
            if (qs->focus != QUICKSCREEN_ITEM_COUNT &&
                (button == ACTION_STD_NEXT || button == ACTION_STD_PREV ||
                 button == ACTION_STD_NEXTREPEAT ||
                 button == ACTION_STD_PREVREPEAT))
            {
                int dir = button == ACTION_STD_NEXT ||
                          button == ACTION_STD_NEXTREPEAT ? 1 : -1;
                do
                    qs->focus = (qs->focus + dir + QUICKSCREEN_ITEM_COUNT) %
                                QUICKSCREEN_ITEM_COUNT;
                while (!qs->items[qs->focus]);
                talk_qs_option(qs, qs->focus, false);
                quickscreen_update(qs, QUICKSCREEN_ITEM_COUNT);
            }
            else if (button == ACTION_STD_OK &&
                     qs->focus != QUICKSCREEN_ITEM_COUNT)
                button = slot_actions[qs->focus];
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
