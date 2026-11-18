/* Native Settings list. Geometry and icon assignments adapted from Scrim
 * by Anthony Fletcher (CC BY-SA 3.0). GPL version 2+. */
#include "config.h"
#include "native_menu.h"
#include "widgets/list.h"

#ifdef IPOD_6G
#include "core_alloc.h"
#include "system.h"
#include "audio.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings/settings.h"
#include "draw/round_rect.h"
#include "draw/viewport.h"
#include "skin/skin_albumart_color.h"
#include "system/activity.h"
#include "widgets/menu.h"
#include "widgets/option_select.h"
#include "screens/settings/exported_settings.h"
#include "native_chrome.h"
#include "native_theme_runtime.h"
#include "retained.h"
#include <string.h>

extern const struct menu_item_ex main_menu_;

struct native_menu_row
{
    struct ui_view view;
    int slot;
};

struct native_menu
{
    struct gui_synclist *list;
    struct ui_theme theme;
    struct native_chrome chrome;
    struct ui_nav nav;
    struct ui_page page;
    struct ui_view root, header, body, marker;
    unsigned generation;
    int rows, list_font;
    int action;
    bool icons;
    unsigned char mark_mask[2];
    struct native_menu_row row[];
};

/* A native menu has a single owner on the UI thread. An unrelated page can
 * temporarily use the legacy painter while that menu's action loop remains. */
static struct gui_synclist *owner;
static struct native_menu *active;
static int allocation;

static unsigned int color(const struct native_menu *menu, enum ui_color_role role)
{
    uint32_t rgb = ui_theme_color(&menu->theme, role, NULL);
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

static bool supported(const struct menu_item_ex *menu)
{
    unsigned int dominant, vivid;
    return ui_native_enabled() && (menu == &main_menu_ || menu == &playback_settings) &&
           get_current_activity() == ACTIVITY_SETTINGS &&
           strcmp(global_settings.sbs_file, "scrim") == 0 &&
           strcmp(global_settings.font_file, "18-noto-sans") == 0 &&
           strcmp(global_settings.bold_font_file, "18-noto-sans-bold") == 0 &&
           global_settings.show_icons && global_settings.scrollbar == SCROLLBAR_OFF &&
           (!global_settings.ui_vp_config[0] ||
            strcmp(global_settings.ui_vp_config, "-") == 0) &&
           (!global_settings.backdrop_file[0] ||
            strcmp(global_settings.backdrop_file, "-") == 0) &&
           !audio_status() && !native_chrome_volume_visible() &&
           !dynamic_colors_roles(&dominant, &vivid);
}

static bool clipped_viewport(struct viewport *vp, struct ui_rect clip,
                             int x, int y, int width, int height)
{
    int right = MIN(clip.x + clip.width, x + width);
    int bottom = MIN(clip.y + clip.height, y + height);
    *vp = *lcd_current_viewport;
    vp->x = MAX(clip.x, x);
    vp->y = MAX(clip.y, y);
    vp->width = right - vp->x;
    vp->height = bottom - vp->y;
    vp->flags = VP_DEFAULT_FLAGS;
    vp->drawmode = DRMODE_FG;
    return vp->width > 0 && vp->height > 0;
}

static void draw_text(struct ui_rect clip, int x, int y, int width, int height,
                      int font, unsigned int color, const char *text)
{
    struct viewport vp;
    if (!text || !clipped_viewport(&vp, clip, x, y, width, height))
        return;
    vp.font = font;
    vp.fg_pattern = color;
    struct viewport *last = lcd_set_viewport(&vp);
    lcd_putsxy(x - vp.x, y - vp.y, text);
    lcd_set_viewport(last);
}

static void paint_body(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct native_menu *menu = view->data;
    struct ui_rect clip = frame->clip;
    struct viewport vp;
    if (!clipped_viewport(&vp, clip, clip.x, clip.y, clip.width, clip.height))
        return;
    vp.fg_pattern = color(menu, UI_COLOR_BACKGROUND);
    struct viewport *last = lcd_set_viewport(&vp);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_viewport(last);
}

static void paint_header(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct native_menu *menu = view->data;
    native_chrome_draw(&menu->chrome, menu->list->title, frame, NULL);
}

static bool equals_lang(const char *value, int id)
{
    return value && strcmp(value, str(id)) == 0;
}

static const char *row_icon(const char *label)
{
    static const struct { int id; const char *glyph; } icons[] = {
        { LANG_SEARCH, "®" }, { LANG_SOUND_SETTINGS, "Y" },
        { LANG_PLAYBACK, "W" }, { LANG_LIBRARY, "v" },
        { LANG_APPEARANCE, "Ê" }, { LANG_BATTERY_POWER, "Á" },
        { LANG_SYSTEM, "Ò" }, { LANG_SETTINGS_MODE, "Í" },
        { LANG_SETTINGS_CHANGED, "É" },
    };
    for (unsigned i = 0; i < ARRAYLEN(icons); ++i)
        if (equals_lang(label, icons[i].id))
            return icons[i].glyph;
    return NULL;
}

static const char *row_accessory(const struct gui_synclist *list, int item,
                                 const char *label)
{
    enum list_row_kind kind = list->callback_get_item_kind(item, list->data);
    char buf[96];
    const char *value = list->callback_get_item_value(item, list->data, buf, sizeof buf);
    if (kind == LIST_ROW_TOGGLE)
    {
        if (equals_lang(value, LANG_SET_BOOL_YES))
            return "ö";
        if (equals_lang(value, LANG_SET_BOOL_NO))
            return "÷";
    }
    int range = list->callback_get_item_range(item, list->data);
    if (range >= 0)
    {
        static const char *const steps[] = { "ø", "ù", "ú", "û", "ü",
                                             "ý", "þ", "ÿ", "Ā", "ā" };
        return steps[range == 0 ? 0 : range >= 100 ? 9 : 1 + range * 8 / 100];
    }
    if (kind == LIST_ROW_SETTING)
    {
        if (equals_lang(label, LANG_BEEP))
        {
            static const struct { int id; const char *glyph; } beep_steps[] = {
                { LANG_OFF, "ø" }, { LANG_WEAK, "û" },
                { LANG_MODERATE, "þ" }, { LANG_STRONG, "ā" },
            };
            for (unsigned i = 0; i < ARRAYLEN(beep_steps); ++i)
                if (equals_lang(value, beep_steps[i].id))
                    return beep_steps[i].glyph;
        }
        if (equals_lang(value, LANG_AUTO))
            return "Ă";
    }
    return kind == LIST_ROW_MENU ? "Þ" : NULL;
}

static void paint_row(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct native_menu *menu = view->data;
    const struct native_menu_row *row = (const struct native_menu_row *)view;
    const struct gui_synclist *list = menu->list;
    int item = list->start_item[SCREEN_MAIN] + row->slot;
    if (item >= list->nb_items)
        return;
    struct ui_rect clip = frame->clip;
    int x = frame->bounds.x, y = frame->bounds.y;
    bool selected = item == list->selected_item;
    unsigned int fg = color(menu, selected ? UI_COLOR_SELECTED_TEXT : UI_COLOR_MUTED);
    const uint16_t *metric = menu->theme.metric;
    int icon_size = metric[UI_METRIC_LIST_ICON_SIZE];
    int icon_margin = metric[UI_METRIC_LIST_ICON_MARGIN];
    int right = frame->bounds.width - metric[UI_METRIC_LIST_RIGHT_PADDING];
    int icon_y = y + (frame->bounds.height - icon_size) / 2;
    int text_height = metric[UI_METRIC_LIST_TEXT_HEIGHT];
    int text_y = y + (frame->bounds.height - text_height) / 2;
    int text_x = metric[UI_METRIC_LIST_TEXT_PADDING];
    char label_buf[128];
    const char *label = P2STR((unsigned char *)list->callback_get_item_name(
                                    item, list->data, label_buf, sizeof label_buf));
    const char *accessory = row_accessory(list, item, label);
    if (menu->icons)
    {
        draw_text(clip, x + icon_margin, icon_y, icon_size, icon_size,
                  menu->chrome.icon_font, fg, row_icon(label));
        text_x += icon_size + metric[UI_METRIC_LIST_ICON_GAP];
    }
    int accessory_x = right - icon_size;
    int text_right = accessory ? accessory_x - metric[UI_METRIC_LIST_ACCESSORY_GAP] : right;
    draw_text(clip, x + text_x, text_y, text_right - text_x, text_height,
              menu->list_font, fg, label);
    draw_text(clip, x + accessory_x, icon_y, icon_size, icon_size,
              menu->chrome.icon_font, fg, accessory);
}

static void paint_marker(const struct ui_view *view, const struct ui_view_frame *frame)
{
    const struct native_menu *menu = view->data;
    struct ui_rect bounds = frame->bounds;
    struct viewport vp;
    if (!clipped_viewport(&vp, frame->clip, bounds.x, bounds.y, bounds.width, bounds.height))
        return;
    vp.fg_pattern = color(menu, UI_COLOR_SELECTION);
    struct viewport *last = lcd_set_viewport(&vp);
    fill_round_rect_aa(&screens[SCREEN_MAIN], bounds.x - vp.x,
                       bounds.y - vp.y, bounds.width, bounds.height,
                       MIN(bounds.width / 2, 1), menu->mark_mask,
                       menu->theme.metric[UI_METRIC_LIST_SELECTION_ALPHA]);
    lcd_set_viewport(last);
}

static bool route_action(struct ui_view *view, int action)
{
    struct native_menu *menu = view->data;
    menu->action = action;
    return gui_synclist_do_button(menu->list, &menu->action);
}

static const struct ui_view_ops body = { .paint = paint_body, .action = route_action };
static const struct ui_view_ops header = { .paint = paint_header };
static const struct ui_view_ops row_painter = { .paint = paint_row };
static const struct ui_view_ops marker_painter = { .paint = paint_marker };

static bool open_menu(struct gui_synclist *list, const struct menu_item_ex *menu,
                      const char *title)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    const uint16_t *metric = theme.metric;
    int header_height = metric[UI_METRIC_STATUS_HEIGHT];
    int list_x = metric[UI_METRIC_LIST_X];
    int list_y = metric[UI_METRIC_LIST_Y];
    int list_width = LCD_WIDTH - 2 * list_x;
    int pitch = metric[UI_METRIC_ROW_HEIGHT];
    int list_height = LCD_HEIGHT - list_y - metric[UI_METRIC_LIST_BOTTOM];
    if (pitch <= 0 || list_height < pitch || list_y < header_height || list_width <= 0)
        return false;
    int rows = list_height / pitch;
    int handle = core_alloc_free_space(sizeof(struct native_menu) +
                                       rows * sizeof(struct native_menu_row));
    if (handle <= 0)
        return false;
    struct native_menu *state = core_get_data_pinned(handle);
    memset(state, 0, sizeof *state);
    state->theme = theme;
    state->generation = ui_theme_generation();
    state->list_font = ui_theme_font_load(&state->theme, UI_FONT_LIST,
                                         global_settings.glyphs_to_cache);
    if (state->list_font < 0)
        goto failed;
    if (!native_chrome_open(&state->chrome, &state->theme))
        goto failed_font;
    native_chrome_poll(&state->chrome);
    if (!native_chrome_title_fits(&state->chrome, title))
    {
        native_chrome_close(&state->chrome);
        goto failed_font;
    }
    state->list = list;
    state->icons = menu == &main_menu_;
    state->rows = rows;
    round_rect_build_mask(state->mark_mask, 1, metric[UI_METRIC_LIST_SELECTION_ALPHA]);
    ui_view_init(&state->root, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT}, NULL, state);
    ui_view_init(&state->header, (struct ui_rect){0, 0, LCD_WIDTH, header_height},
                 &header, state);
    ui_view_init(&state->body, (struct ui_rect){0, header_height, LCD_WIDTH,
                  LCD_HEIGHT - header_height}, &body, state);
    ui_view_append(&state->root, &state->header);
    ui_view_append(&state->root, &state->body);
    for (int i = 0; i < rows; ++i)
    {
        state->row[i].slot = i;
        ui_view_init(&state->row[i].view, (struct ui_rect){list_x,
                     list_y - header_height + i * pitch, list_width, pitch},
                     &row_painter, state);
        ui_view_append(&state->body, &state->row[i].view);
    }
    ui_view_init(&state->marker, (struct ui_rect){0, 0, 0, pitch}, &marker_painter, state);
    ui_view_append(&state->body, &state->marker);
    ui_page_init(&state->page, &state->root);
    state->page.focus = &state->body;
    ui_nav_push(&state->nav, &state->page, UI_PAGE);
    active = state;
    allocation = handle;
    return true;

failed_font:
    font_unload(state->list_font);
failed:
    core_put_data_pinned(state);
    core_free(handle);
    return false;
}

static void close_menu(void)
{
    struct native_menu *state = active;
    active = NULL;
    ui_nav_pop(&state->nav);
    native_chrome_close(&state->chrome);
    font_unload(state->list_font);
    core_put_data_pinned(state);
    allocation = core_free(allocation);
}

bool native_menu_begin(struct gui_synclist *list, const struct menu_item_ex *menu,
                       const char *title)
{
    if (owner || !supported(menu) || !open_menu(list, menu, title))
        return false;
    owner = list;
    return true;
}

void native_menu_suspend(struct gui_synclist *list)
{
    if (!native_menu_active(list))
        return;
    close_menu();
    /* The base menu frame is unthemed; the override lasts only for as long
     * as this menu shows a legacy screen. */
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
}

bool native_menu_update(struct gui_synclist *list, const struct menu_item_ex *menu,
                        const char *title)
{
    if (owner != list)
        return false;
    if (supported(menu))
    {
        if (active && active->generation != ui_theme_generation())
        {
            close_menu();
            if (open_menu(list, menu, title))
                return true;
            viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
            return true;
        }
        if (!active)
        {
            if (!open_menu(list, menu, title))
                return false;
            viewportmanager_theme_undo(SCREEN_MAIN, false);
            return true;
        }
        if (native_chrome_title_fits(&active->chrome, title))
        {
            active->icons = menu == &main_menu_;
            return false;
        }
    }
    if (active)
    {
        native_menu_suspend(list);
        return true;
    }
    return false;
}

void native_menu_end(struct gui_synclist *list)
{
    if (owner != list)
        return;
    if (active)
        close_menu();
    else
        viewportmanager_theme_undo(SCREEN_MAIN, false);
    owner = NULL;
}

bool native_menu_active(const struct gui_synclist *list)
{
    return active && owner == list;
}

bool native_menu_draw(struct gui_synclist *list)
{
    if (!native_menu_active(list))
        return false;
    native_chrome_poll(&active->chrome);
    const uint16_t *metric = active->theme.metric;
    int selected = list->selected_item - list->start_item[SCREEN_MAIN];
    int x = metric[UI_METRIC_LIST_X] + metric[UI_METRIC_LIST_ICON_MARGIN];
    if (active->icons)
        x += metric[UI_METRIC_LIST_ICON_SIZE] + metric[UI_METRIC_LIST_ICON_GAP];
    int y = metric[UI_METRIC_LIST_Y] - metric[UI_METRIC_STATUS_HEIGHT] +
            selected * metric[UI_METRIC_ROW_HEIGHT];
    int width = list->nb_items && selected >= 0 && selected < active->rows ?
                metric[UI_METRIC_LIST_SELECTION_WIDTH] : 0;
    ui_page_set(&active->page, &active->marker, UI_X, x);
    ui_page_set(&active->page, &active->marker, UI_Y, y);
    ui_page_set(&active->page, &active->marker, UI_WIDTH, width);
    ui_page_invalidate(&active->page, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT});
    if (gui_synclist_flush_inhibited())
        ui_page_paint(&active->page);
    else
        ui_nav_draw(&active->nav);
    return true;
}

void native_menu_tick(const struct gui_synclist *list)
{
    if (native_menu_active(list) && native_chrome_poll(&active->chrome))
    {
        ui_page_invalidate(&active->page, (struct ui_rect){0, 0, LCD_WIDTH,
                           active->theme.metric[UI_METRIC_STATUS_HEIGHT]});
        ui_page_draw(&active->page);
    }
}

int native_menu_lines(const struct gui_synclist *list)
{
    return native_menu_active(list) ? active->rows : 0;
}

int native_menu_status_height(void)
{
    return active ? active->theme.metric[UI_METRIC_STATUS_HEIGHT] : 0;
}

bool native_menu_button(struct gui_synclist *list, int *action)
{
    if (!native_menu_active(list))
        return gui_synclist_do_button(list, action);
    bool handled = ui_nav_dispatch(&active->nav, *action);
    *action = active->action;
    return handled;
}
#else
bool native_menu_begin(struct gui_synclist *list, const struct menu_item_ex *menu,
                       const char *title)
{ (void)list; (void)menu; (void)title; return false; }

bool native_menu_update(struct gui_synclist *list, const struct menu_item_ex *menu,
                        const char *title)
{ (void)list; (void)menu; (void)title; return false; }

void native_menu_suspend(struct gui_synclist *list) { (void)list; }
void native_menu_end(struct gui_synclist *list) { (void)list; }
bool native_menu_active(const struct gui_synclist *list) { (void)list; return false; }
bool native_menu_draw(struct gui_synclist *list) { (void)list; return false; }
void native_menu_tick(const struct gui_synclist *list) { (void)list; }
int native_menu_lines(const struct gui_synclist *list) { (void)list; return 0; }
int native_menu_status_height(void) { return 0; }
bool native_menu_button(struct gui_synclist *list, int *action)
{ return gui_synclist_do_button(list, action); }
#endif
