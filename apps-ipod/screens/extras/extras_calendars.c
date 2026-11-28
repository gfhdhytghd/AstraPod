/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Calendars and To Do's: reads and edits RFC 5545 VEVENT and
 * VTODO components across /Calendars .ics files, creating new items in
 * /Calendars/AstraPod.ics with atomic writes and byte-for-byte preservation
 * of unknown properties.
 *
 * Draws the stock 2.0.5 look in Classic / Fruit Company / Fruit Company
 * Ultra and uses Scrim's native theme palette in Scrim.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "kernel.h"
#include "core_alloc.h"
#include "file.h"
#include "font.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "input/action.h"
#include "button.h"
#include "draw/screen_access.h"
#include "draw/text_wrap.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "ui/theme_art.h"
#include "widgets/keyboard.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "root_menu.h"
#include "extras_calendars.h"
#include "extras_ical.h"
#include "extras_picker.h"
#include "extras_store.h"
#include "extras.h"
#include "extras_font.h"

#define CAL_FONT FONT_DIR "/14-helvetica-bold-calendar.pbf"

#define WHITE 0xffffff
#define BLACK 0x000000
#define GREY  0x666666

/* The calendar page: its size, month baseline, and day origin. */
#define ICON_W      130
#define MONTH_BASE  36
#define DAY_X       40
#define DAY_BASE    90
#define DIGIT_W     27
#define DIGIT_TOP   35
#define PANE_ICON_X 176
#define PANE_ICON_Y 56
#define PANE_MID    (MENU_PANE_X + (LCD_WIDTH - MENU_PANE_X) / 2)
#define TIME_BASE   34
#define WEEKDAY_BASE 221

/* Month grid geometry. */
#define CELL_X      4
#define CELL_Y      38
#define CELL_DX     45
#define CELL_DY     34
#define CELL_W      41
#define CELL_H      30
#define HEAD_BASE   34
#define NUM_X       4
#define NUM_BASE    15
#define FLAG_X      26
#define FLAG_Y      4
#define BAND_ROWS   16

/* Tall two-line rows (stock 2.0.5 Day and To Do lists). */
#define TALL_ROW_H      44
#define TALL_TEXT_X     8
#define TALL_TITLE_BASE 18
#define TALL_SUB_BASE   35
#define TALL_CHEVRON_R  16
#define TALL_CHEVRON_Y  15
#define SUB_RGB         LCD_RGBPACK(107, 109, 107)
#define ROW_RULE_RGB    LCD_RGBPACK(222, 223, 222)

#define DAY_EVENTS_MAX  64

static const char *const repeat_labels[ICAL_REPEAT_COUNT] = {
    "None", "Daily", "Weekly", "Monthly", "Yearly",
};

static const char *const alarm_labels[ICAL_ALARM_COUNT] = {
    "None", "At time", "5 min before", "15 min before",
    "30 min before", "1 hour before", "1 day before",
};

static const char *const alarm_short_labels[ICAL_ALARM_COUNT] = {
    "None", "At time", "5 min", "15 min", "30 min", "1 hour", "1 day",
};

static const char *const priority_labels[4] = {
    "None", "Low", "Medium", "High",
};

static bool is_stock_look(void)
{
    return ui_theme_style(UI_STYLE_ART) == UI_ART_PLASTIC && stock_art_ready();
}

static uint32_t theme_rgb24(enum ui_color_role role)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    return ui_theme_color(&theme, role, NULL);
}

static void format_date_lang(char *buf, size_t size, int lang,
                             const char *month, int day, int year)
{
    size_t used = 0;
    buf[0] = '\0';
    for (const char *f = str(lang); *f && used < size; f++)
    {
        if (*f == 'M')
            used += snprintf(buf + used, size - used, "%s", month);
        else if (*f == 'D')
            used += snprintf(buf + used, size - used, "%d", day);
        else if (*f == 'Y')
            used += snprintf(buf + used, size - used, "%d", year);
        else
            used += snprintf(buf + used, size - used, "%c", *f);
    }
}

static void format_short_date(char *buf, size_t size,
                              const struct extras_date *d)
{
    snprintf(buf, size, "%s %d, %04d",
             str(LANG_MONTH_JANUARY + d->month - 1), d->day, d->year);
}

/* ---- Calendar icon and split pane ---- */

static int pane_font(void)
{
    static int font = -1;
    static unsigned generation;
    if (font < 0 || generation != ui_theme_generation())
    {
        font_unload(font);
        font = ui_theme_current_font_load(UI_FONT_PANE_TITLE,
                                          global_settings.glyphs_to_cache);
        generation = ui_theme_generation();
    }
    return font;
}

static int text_width(int font, const char *text)
{
    int w = 0;
    font_getstringsize((const unsigned char *)text, &w, NULL, font);
    return w;
}

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int base, uint32_t rgb)
{
    if (font >= 0)
        stock_canvas_text(c, font, text, x, base - font_get(font)->ascent,
                          rgb);
}

static void put_digit(struct stock_canvas *c, int n, int x, int base)
{
    const struct stock_art *a = &stock_art[STOCK_ART_ASTRAPOD_CALENDARDIGITS];
    if (!stock_art_ready())
        return;
    for (int row = 0; row < a->height; row++)
    {
        int y = base - DIGIT_TOP + row - c->y;
        if (y < 0 || y >= c->height)
            continue;
        for (int col = 0; col < DIGIT_W; col++)
        {
            unsigned cover = stock_art_alpha(a, n * DIGIT_W + col, row);
            int cx = x + col - c->x;
            if (!cover || cx < 0 || cx >= c->width)
                continue;
            uint32_t *p = c->pixels + y * c->width + cx;
            uint32_t out = 0;
            for (int shift = 0; shift < 24; shift += 8)
                out |= (((*p >> shift & 255) * (255 - cover) + 1) * 257 >> 16)
                       << shift;
            *p = out;
        }
    }
}

void extras_calendars_icon_put(struct stock_canvas *canvas, int x, int y,
                               int month, int day)
{
    int font = pane_font();
    const char *name = str(LANG_MONTH_JANUARY + month - 1);
    stock_art_put(canvas, STOCK_ART_SETTINGS_DATETIME_CALENDAR, x, y);
    put_text(canvas, font, name, x + (ICON_W - text_width(font, name)) / 2,
             y + MONTH_BASE, WHITE);
    put_digit(canvas, day / 10, x + DAY_X, y + DAY_BASE);
    put_digit(canvas, day % 10, x + DAY_X + DIGIT_W, y + DAY_BASE);
}

static void put_centred(struct stock_canvas *c, int font, const char *text,
                        int mid, int base, uint32_t rgb)
{
    put_text(c, font, text, mid - text_width(font, text) / 2, base, rgb);
}

static void pane_paint(struct stock_canvas *band)
{
    static const int weekday[7] = {
        LANG_EXTRAS_CALENDARS_SUNDAY, LANG_EXTRAS_CALENDARS_MONDAY,
        LANG_EXTRAS_CALENDARS_TUESDAY, LANG_EXTRAS_CALENDARS_WEDNESDAY,
        LANG_EXTRAS_CALENDARS_THURSDAY, LANG_EXTRAS_CALENDARS_FRIDAY,
        LANG_EXTRAS_CALENDARS_SATURDAY,
    };
    const struct tm *tm = get_time();
    struct extras_date d = extras_today();
    char time_buf[16];
    extras_format_time(time_buf, sizeof(time_buf), tm->tm_hour, tm->tm_min);
    int font = pane_font();
    put_centred(band, font, time_buf, PANE_MID, TIME_BASE, WHITE);
    extras_calendars_icon_put(band, PANE_ICON_X, PANE_ICON_Y, d.month, d.day);
    put_centred(band, font, str(weekday[extras_week_day(&d)]), PANE_MID,
                WEEKDAY_BASE, WHITE);
}

void extras_calendars_pane(struct menu_pane_content *c)
{
    const struct tm *tm = get_time();
    memset(c, 0, sizeof(*c));
    c->kind = LIST_PANE_PICTURE;
    c->art = -1;
    c->paint = pane_paint;
    c->stamp = (tm->tm_yday * 24 + tm->tm_hour) * 60 + tm->tm_min;
}

/* ---- Shared choice sheet & confirmation helpers ---- */

struct choice_ctx {
    const char *const *items;
    int count;
};

static const char *choice_get_name(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct choice_ctx *c = data;
    return c->items[i];
}

static enum list_row_kind choice_get_kind(int i, void *data)
{
    (void)i; (void)data;
    return LIST_ROW_OPTION;
}

/* Returns selected index 0..count-1, -1 on cancel/back, -2 on USB. */
static int show_choice_list(const char *title, const char *const *items,
                            int count, int initial)
{
    struct choice_ctx ctx = { items, count };
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int action, ret = -1;

    ui_scene_open_page(&scene, "cal-choice-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_init(&list, choice_get_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, choice_get_kind, NULL, NULL);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_select_item(&list, MIN(MAX(initial, 0), count - 1));
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        if (action == ACTION_STD_OK)
        {
            ret = gui_synclist_get_sel_pos(&list);
            break;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = -2;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret != -2, "cal-choice-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/* Returns 1 on confirm, 0 on cancel, -1 on USB. */
static int confirm_prompt(const char *question, int yes_lang, int no_lang)
{
    const char *lines[1] = { question };
    struct text_message msg = { lines, 1 };
    enum yesno_res r = gui_syncyesno_run_labels(&msg, yes_lang, no_lang);
    if (r == YESNO_USB)
        return -1;
    return r == YESNO_YES ? 1 : r == YESNO_CANCEL ? 2 : 0;
}

/* ---- Event Editor ---- */

enum ev_field {
    EVF_TITLE, EVF_DATE, EVF_ALL_DAY, EVF_START, EVF_END,
    EVF_REPEAT, EVF_ALARM, EVF_NOTES, EVF_SAVE, EVF_DELETE
};

struct ev_editor_ctx {
    struct ical_event *ev;
    enum ev_field rows[10];
    int row_count;
    bool dirty;
};

static struct ical_event *pane_ev;

static void event_pane_paint(struct stock_canvas *band)
{
    if (!pane_ev)
        return;
    int font = pane_font();
    char top[32], bot[32];
    if (pane_ev->all_day)
        strlcpy(top, "All Day", sizeof(top));
    else
        extras_format_time(top, sizeof(top), pane_ev->start_hour,
                           pane_ev->start_min);
    strlcpy(bot, repeat_labels[pane_ev->repeat], sizeof(bot));
    put_centred(band, font, top, PANE_MID, TIME_BASE, WHITE);
    extras_calendars_icon_put(band, PANE_ICON_X, PANE_ICON_Y,
                              pane_ev->date.month, pane_ev->date.day);
    put_centred(band, font, bot, PANE_MID, WEEKDAY_BASE, WHITE);
}

static enum list_pane_kind ev_pane_cb(int i, void *data, int *art)
{
    (void)i; (void)art;
    struct ev_editor_ctx *ctx = data;
    pane_ev = ctx->ev;
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = -1, .paint = event_pane_paint,
        .stamp = (ctx->ev->date.month * 32 + ctx->ev->date.day) * 2048 +
                 (ctx->ev->all_day ? 1 : 0) * 1024 +
                 ctx->ev->start_hour * 32 + ctx->ev->repeat,
    };
    menu_pane_set_picture(&c);
    return LIST_PANE_PICTURE;
}

static void ev_rebuild_rows(struct ev_editor_ctx *ctx)
{
    int n = 0;
    ctx->rows[n++] = EVF_TITLE;
    ctx->rows[n++] = EVF_DATE;
    ctx->rows[n++] = EVF_ALL_DAY;
    if (!ctx->ev->all_day)
    {
        ctx->rows[n++] = EVF_START;
        ctx->rows[n++] = EVF_END;
    }
    ctx->rows[n++] = EVF_REPEAT;
    ctx->rows[n++] = EVF_ALARM;
    ctx->rows[n++] = EVF_NOTES;
    ctx->rows[n++] = EVF_SAVE;
    if (ctx->ev->is_existing)
        ctx->rows[n++] = EVF_DELETE;
    ctx->row_count = n;
}

static const char *ev_row_name(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct ev_editor_ctx *ctx = data;
    switch (ctx->rows[i])
    {
        case EVF_TITLE:   return "Title";
        case EVF_DATE:    return str(LANG_DATE);
        case EVF_ALL_DAY: return "All Day";
        case EVF_START:   return "Start";
        case EVF_END:     return "End";
        case EVF_REPEAT:  return str(LANG_REPEAT);
        case EVF_ALARM:   return "Alarm";
        case EVF_NOTES:   return "Notes";
        case EVF_SAVE:    return str(LANG_SAVE);
        case EVF_DELETE:  return "Delete Event";
        default:          return "";
    }
}

static const char *ev_row_value(int i, void *data, char *buf, size_t size)
{
    struct ev_editor_ctx *ctx = data;
    const struct ical_event *ev = ctx->ev;
    switch (ctx->rows[i])
    {
        case EVF_TITLE:
            return ev->summary[0] ? ev->summary : "Untitled";
        case EVF_DATE:
            format_short_date(buf, size, &ev->date);
            return buf;
        case EVF_ALL_DAY:
            return str(ev->all_day ? LANG_ON : LANG_OFF);
        case EVF_START:
            extras_format_time(buf, size, ev->start_hour, ev->start_min);
            return buf;
        case EVF_END:
            extras_format_time(buf, size, ev->end_hour, ev->end_min);
            return buf;
        case EVF_REPEAT:
            return repeat_labels[ev->repeat];
        case EVF_ALARM:
            return alarm_short_labels[ev->alarm];
        case EVF_NOTES:
            return ev->notes[0] ? ev->notes : "None";
        default:
            return NULL;
    }
}

static enum list_row_kind ev_row_kind(int i, void *data)
{
    struct ev_editor_ctx *ctx = data;
    switch (ctx->rows[i])
    {
        case EVF_ALL_DAY: return LIST_ROW_TOGGLE;
        case EVF_SAVE:
        case EVF_DELETE:  return LIST_ROW_ACTION;
        default:          return LIST_ROW_SETTING;
    }
}

/* Returns 0 normally (saved or cancelled), GO_TO_ROOT on USB. Sets *changed. */
static int run_event_editor(struct ical_event *ev, bool *changed)
{
    struct ev_editor_ctx ctx = { .ev = ev, .dirty = false };
    struct gui_synclist list;
    struct ui_scene scene = {0};
    const char *title = ev->is_existing ? "Edit Event" : "New Event";
    int sel = 0, action, ret = 0;

    ev_rebuild_rows(&ctx);
    ui_scene_open_page(&scene, "cal-ev-edit-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);

    gui_synclist_init(&list, ev_row_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, ev_row_kind, ev_row_value, NULL);
    gui_synclist_set_pane_callback(&list, ev_pane_cb);
    gui_synclist_set_nb_items(&list, ctx.row_count);
    gui_synclist_select_item(&list, sel);
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        sel = gui_synclist_get_sel_pos(&list);

        if (action == ACTION_STD_OK)
        {
            enum ev_field f = ctx.rows[sel];
            if (f == EVF_TITLE)
            {
                char tmp[sizeof(ev->summary)];
                strlcpy(tmp, ev->summary, sizeof(tmp));
                if (dialog_input(tmp, sizeof(tmp)) == 0)
                {
                    strlcpy(ev->summary, tmp, sizeof(ev->summary));
                    ctx.dirty = true;
                }
            }
            else if (f == EVF_DATE)
            {
                int r = extras_pick_date("Event Date", ev->summary,
                                         &ev->date, 2000, 2099);
                if (r < 0) { ret = GO_TO_ROOT; break; }
                if (r > 0) ctx.dirty = true;
            }
            else if (f == EVF_ALL_DAY)
            {
                ev->all_day = !ev->all_day;
                ctx.dirty = true;
                ev_rebuild_rows(&ctx);
                gui_synclist_set_nb_items(&list, ctx.row_count);
                gui_synclist_select_item(&list, MIN(sel, ctx.row_count - 1));
            }
            else if (f == EVF_START)
            {
                int r = extras_pick_time("Start Time", ev->summary,
                                         &ev->start_hour, &ev->start_min);
                if (r < 0) { ret = GO_TO_ROOT; break; }
                if (r > 0)
                {
                    int st = ev->start_hour * 60 + ev->start_min;
                    int en = ev->end_hour * 60 + ev->end_min;
                    if (en <= st)
                    {
                        en = (st + 60) % (24 * 60);
                        ev->end_hour = en / 60;
                        ev->end_min = en % 60;
                    }
                    ctx.dirty = true;
                }
            }
            else if (f == EVF_END)
            {
                int r = extras_pick_time("End Time", ev->summary,
                                         &ev->end_hour, &ev->end_min);
                if (r < 0) { ret = GO_TO_ROOT; break; }
                if (r > 0) ctx.dirty = true;
            }
            else if (f == EVF_REPEAT)
            {
                int r = show_choice_list("Repeat", repeat_labels,
                                         ICAL_REPEAT_COUNT, ev->repeat);
                if (r == -2) { ret = GO_TO_ROOT; break; }
                if (r >= 0) { ev->repeat = (enum ical_repeat)r; ctx.dirty = true; }
            }
            else if (f == EVF_ALARM)
            {
                int r = show_choice_list("Alarm", alarm_labels,
                                         ICAL_ALARM_COUNT, ev->alarm);
                if (r == -2) { ret = GO_TO_ROOT; break; }
                if (r >= 0) { ev->alarm = (enum ical_alarm)r; ctx.dirty = true; }
            }
            else if (f == EVF_NOTES)
            {
                char tmp[sizeof(ev->notes)];
                strlcpy(tmp, ev->notes, sizeof(tmp));
                if (dialog_input(tmp, sizeof(tmp)) == 0)
                {
                    strlcpy(ev->notes, tmp, sizeof(ev->notes));
                    ctx.dirty = true;
                }
            }
            else if (f == EVF_SAVE)
            {
                if (ical_save_event(ICAL_DIR, ev))
                {
                    *changed = true;
                    break;
                }
                splash(HZ, "Could not save event");
            }
            else if (f == EVF_DELETE)
            {
                int c = confirm_prompt("Delete this event?",
                                       LANG_DELETE, LANG_DIAG_CANCEL);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 1)
                {
                    if (ical_delete_component(ev->path, ev->comp_start,
                                              ev->comp_end))
                    {
                        *changed = true;
                        break;
                    }
                    splash(HZ, "Could not delete event");
                }
            }
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            if (ctx.dirty)
            {
                int c = confirm_prompt("Save changes?",
                                       LANG_SAVE, LANG_EXTRAS_DISCARD);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 2) continue;
                if (c == 1)
                {
                    if (!ical_save_event(ICAL_DIR, ev))
                    {
                        splash(HZ, "Could not save event");
                        gui_synclist_draw(&list);
                        continue;
                    }
                    *changed = true;
                }
            }
            break;
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "cal-ev-edit-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    pane_ev = NULL;
    return ret;
}

/* ---- Day View ---- */

struct day_ctx {
    struct ical_index *idx;
    struct extras_date shown;
    int events[DAY_EVENTS_MAX];
    int count;
    int sub_font;
};

static void day_title(const struct extras_date *d, char *buf, size_t size)
{
    char month[24];
    strlcpy(month, str(LANG_EXTRAS_CALENDARS_JANUARY + d->month - 1),
            sizeof(month));
    format_date_lang(buf, size, LANG_EXTRAS_CALENDARS_DAY_TITLE,
                     month, d->day, d->year);
}

static const char *day_get_name(int row, void *data, char *buf, size_t size)
{
    struct day_ctx *ctx = data;
    const struct ical_event_idx *e = &ctx->idx->events[ctx->events[row]];
    const char *s = ical_pool_str(ctx->idx, e->summary_off);
    strlcpy(buf, *s ? s : "Untitled", size);
    return buf;
}

static enum list_row_kind day_get_kind(int row, void *data)
{
    (void)row; (void)data;
    return LIST_ROW_MENU;
}

static void day_subtitle(const struct ical_event_idx *e, char *buf, size_t size)
{
    char t0[16], t1[16];
    if (e->all_day)
        strlcpy(buf, "All Day", size);
    else
    {
        extras_format_time(t0, sizeof(t0), e->start_hour, e->start_min);
        extras_format_time(t1, sizeof(t1), e->end_hour, e->end_min);
        snprintf(buf, size, "%s - %s", t0, t1);
    }
    if (e->repeat != ICAL_REPEAT_NONE)
    {
        size_t len = strlen(buf);
        snprintf(buf + len, size - len, "  (%s)", repeat_labels[e->repeat]);
    }
    if (e->alarm != ICAL_ALARM_NONE)
    {
        size_t len = strlen(buf);
        snprintf(buf + len, size - len, "  [Alarm]");
    }
}

static void draw_day_row(struct list_putlineinfo_t *info)
{
    struct day_ctx *ctx = info->list->data;
    struct viewport *vp = info->vp;
    const struct viewport *parent = info->list->parent[SCREEN_MAIN];
    int width = parent->x + parent->width - (vp->x + info->x);
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    int x = info->x + TALL_TEXT_X;
    bool stock = is_stock_look();

    if (info->is_selected)
    {
        const uint32_t *bar = stock ? ui_art_selection(TALL_ROW_H) : NULL;
        if (bar)
        {
            for (int y = 0; y < TALL_ROW_H; y++)
            {
                vp->fg_pattern = LCD_RGBPACK(bar[y] >> 16, (bar[y] >> 8) & 255,
                                             bar[y] & 255);
                lcd_hline(info->x, info->x + width - 1, info->y + y);
            }
        }
        else
        {
            vp->fg_pattern = LCD_RGBPACK(30, 34, 44);
            lcd_fillrect(info->x, info->y, width, TALL_ROW_H);
            vp->fg_pattern = ui_theme_rgb565(UI_COLOR_SELECTION);
            lcd_fillrect(info->x, info->y + 4, 4, TALL_ROW_H - 8);
        }
    }

    vp->fg_pattern = info->is_selected ?
        (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
        (stock ? fg : ui_theme_rgb565(UI_COLOR_FOREGROUND));
    lcd_putsxy(x, info->y + TALL_TITLE_BASE - font_get(font)->ascent,
               (const unsigned char *)info->dsp_text);

    if (ctx->sub_font >= 0)
    {
        char sub[80];
        const struct ical_event_idx *e =
            &ctx->idx->events[ctx->events[info->line]];
        day_subtitle(e, sub, sizeof(sub));
        vp->font = ctx->sub_font;
        vp->fg_pattern = info->is_selected ?
            (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
            (stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED));
        lcd_putsxy(x, info->y + TALL_SUB_BASE - font_get(ctx->sub_font)->ascent,
                   (const unsigned char *)sub);
        vp->font = font;
    }

    if (info->is_selected && stock)
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU,
                       vp->x + info->x + width - TALL_CHEVRON_R,
                       vp->y + info->y + TALL_CHEVRON_Y,
                       (struct ui_rect){ vp->x, vp->y, vp->width, vp->height });
    else if (!info->is_selected)
    {
        vp->fg_pattern = stock ? ROW_RULE_RGB :
                                 ui_theme_rgb565(UI_COLOR_SEPARATOR);
        lcd_hline(info->x, info->x + width - 1, info->y + TALL_ROW_H - 1);
    }
    vp->fg_pattern = fg;
}

static void paint_empty_page(const char *heading, const char *hint, int font,
                             int sub_font)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport content;
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    bool stock = is_stock_look();
    content.bg_pattern = stock ? LCD_WHITE :
                                 ui_theme_rgb565(UI_COLOR_BACKGROUND);
    content.fg_pattern = stock ? LCD_BLACK :
                                 ui_theme_rgb565(UI_COLOR_FOREGROUND);
    content.drawmode = DRMODE_SOLID;
    struct viewport *last = d->set_viewport(&content);
    d->clear_viewport();

    int f = font >= 0 ? font : content.font;
    content.font = f;
    content.drawmode = DRMODE_FG;
    int w = 0, h = 0;
    font_getstringsize((const unsigned char *)heading, &w, &h, f);
    d->putsxy((content.width - w) / 2, content.height / 2 - h,
              (const unsigned char *)heading);

    if (hint && *hint)
    {
        int sf = sub_font >= 0 ? sub_font : f;
        content.font = sf;
        content.fg_pattern = stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED);
        font_getstringsize((const unsigned char *)hint, &w, &h, sf);
        d->putsxy((content.width - w) / 2, content.height / 2 + 10,
                  (const unsigned char *)hint);
    }

    d->set_viewport(last);
    d->update();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
}

static int calendar_button_step(int action)
{
    if (action != ACTION_STD_CANCEL && action != ACTION_STD_OK)
        return 0;
    int button;
    get_action_statuscode(&button);
    return button == BUTTON_LEFT ? -1 : button == BUTTON_RIGHT ? 1 : 0;
}

static int day_page(struct ical_index *idx, struct extras_date *d)
{
    struct day_ctx ctx = { .idx = idx, .shown = *d };
    char title[48];
    int ret = 0, sel = 0;
    bool select_wheeled = false;
    struct ui_scene scene = {0};

    ctx.sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                              global_settings.glyphs_to_cache);
    int head_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                               global_settings.glyphs_to_cache);

    ui_scene_open_page(&scene, "cal-day-push");

    for (;;)
    {
        ctx.count = ical_events_on_day(idx, &ctx.shown, ctx.events,
                                       DAY_EVENTS_MAX);
        day_title(&ctx.shown, title, sizeof(title));
        sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);

        if (ctx.count == 0)
        {
            paint_empty_page(str(LANG_EXTRAS_CALENDARS_NO_EVENTS),
                             "SELECT: New Event", head_font, ctx.sub_font);
            int action = get_action(CONTEXT_LIST, HZ);
            int step = calendar_button_step(action);
            if (step)
            {
                extras_step_days(&ctx.shown, step);
                continue;
            }
            if (!(button_status() & BUTTON_SELECT))
                select_wheeled = false;
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
                break;
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                break;
            }
            if ((action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT) &&
                (button_status() & BUTTON_SELECT))
            {
                select_wheeled = true;
                extras_step_days(&ctx.shown, 1);
                continue;
            }
            if ((action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT) &&
                (button_status() & BUTTON_SELECT))
            {
                select_wheeled = true;
                extras_step_days(&ctx.shown, -1);
                continue;
            }
            if (action == ACTION_STD_OK)
            {
                struct ical_event ev;
                bool changed = false;
                ical_event_init_new(&ev, &ctx.shown);
                ret = run_event_editor(&ev, &changed);
                if (ret) break;
                if (changed) ical_scan(idx, ICAL_DIR);
            }
            else if (action == ACTION_STD_CONTEXT && !select_wheeled)
            {
                static const char *const menu_empty[] = {
                    "New Event", "Next Day", "Previous Day", "Go to Today"
                };
                int choice = show_choice_list(title, menu_empty,
                                              ARRAYLEN(menu_empty), 0);
                if (choice == -2) { ret = GO_TO_ROOT; break; }
                if (choice == 0)
                {
                    struct ical_event ev;
                    bool changed = false;
                    ical_event_init_new(&ev, &ctx.shown);
                    ret = run_event_editor(&ev, &changed);
                    if (ret) break;
                    if (changed) ical_scan(idx, ICAL_DIR);
                }
                else if (choice == 1) extras_step_days(&ctx.shown, 1);
                else if (choice == 2) extras_step_days(&ctx.shown, -1);
                else if (choice == 3) ctx.shown = extras_today();
            }
            continue;
        }

        struct gui_synclist list;
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        gui_synclist_init(&list, day_get_name, &ctx, false, 1, NULL);
        gui_synclist_set_title(&list, title, Icon_NOICON);
        gui_synclist_set_row_callbacks(&list, day_get_kind, NULL, NULL);
        gui_synclist_set_row_height(&list, TALL_ROW_H);
        list.callback_draw_item = draw_day_row;
        gui_synclist_set_nb_items(&list, ctx.count);
        gui_synclist_select_item(&list, MIN(sel, ctx.count - 1));
        gui_synclist_draw_settled(&list);

        bool reload = false, leave = false;
        while (!reload && !leave)
        {
            int action;
            bool handled = list_do_action(CONTEXT_LIST, HZ, &list, &action);
            int step = calendar_button_step(action);
            if (step)
            {
                extras_step_days(&ctx.shown, step);
                sel = 0;
                reload = true;
                break;
            }
            if (!(button_status() & BUTTON_SELECT))
                select_wheeled = false;

            if ((action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT) &&
                (button_status() & BUTTON_SELECT))
            {
                select_wheeled = true;
                extras_step_days(&ctx.shown, 1);
                sel = 0;
                reload = true;
                break;
            }
            if ((action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT) &&
                (button_status() & BUTTON_SELECT))
            {
                select_wheeled = true;
                extras_step_days(&ctx.shown, -1);
                sel = 0;
                reload = true;
                break;
            }
            if (handled)
            {
                sel = gui_synclist_get_sel_pos(&list);
                continue;
            }
            sel = gui_synclist_get_sel_pos(&list);

            if (action == ACTION_STD_OK)
            {
                const struct ical_event_idx *ei = &idx->events[ctx.events[sel]];
                char path[MAX_PATH];
                ical_file_path(idx, ei->file_idx, path, sizeof(path));
                struct ical_event ev;
                if (ical_load_event(path, ei->comp_start, ei->comp_end, &ev))
                {
                    bool changed = false;
                    gui_synclist_scroll_stop(&list);
                    viewportmanager_theme_undo(SCREEN_MAIN, false);
                    ret = run_event_editor(&ev, &changed);
                    if (ret) goto finish;
                    if (changed) ical_scan(idx, ICAL_DIR);
                    reload = true;
                    goto next_iter;
                }
            }
            else if (action == ACTION_STD_CONTEXT && !select_wheeled)
            {
                static const char *const menu_day[] = {
                    "New Event", "Edit Event", "Delete Event",
                    "Next Day", "Previous Day", "Go to Today"
                };
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                int choice = show_choice_list(title, menu_day,
                                              ARRAYLEN(menu_day), 0);
                if (choice == -2) { ret = GO_TO_ROOT; goto finish; }
                if (choice == 0)
                {
                    struct ical_event ev;
                    bool changed = false;
                    ical_event_init_new(&ev, &ctx.shown);
                    ret = run_event_editor(&ev, &changed);
                    if (ret) goto finish;
                    if (changed) ical_scan(idx, ICAL_DIR);
                }
                else if (choice == 1)
                {
                    const struct ical_event_idx *ei =
                        &idx->events[ctx.events[sel]];
                    char path[MAX_PATH];
                    ical_file_path(idx, ei->file_idx, path, sizeof(path));
                    struct ical_event ev;
                    if (ical_load_event(path, ei->comp_start, ei->comp_end, &ev))
                    {
                        bool changed = false;
                        ret = run_event_editor(&ev, &changed);
                        if (ret) goto finish;
                        if (changed) ical_scan(idx, ICAL_DIR);
                    }
                }
                else if (choice == 2)
                {
                    int c = confirm_prompt("Delete this event?",
                                           LANG_DELETE, LANG_DIAG_CANCEL);
                    if (c < 0) { ret = GO_TO_ROOT; goto finish; }
                    if (c == 1)
                    {
                        const struct ical_event_idx *ei =
                            &idx->events[ctx.events[sel]];
                        char path[MAX_PATH];
                        ical_file_path(idx, ei->file_idx, path, sizeof(path));
                        if (ical_delete_component(path, ei->comp_start,
                                                  ei->comp_end))
                            ical_scan(idx, ICAL_DIR);
                    }
                }
                else if (choice == 3) { extras_step_days(&ctx.shown, 1); sel = 0; }
                else if (choice == 4) { extras_step_days(&ctx.shown, -1); sel = 0; }
                else if (choice == 5) { ctx.shown = extras_today(); sel = 0; }
                reload = true;
                goto next_iter;
            }
            else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            {
                leave = true;
            }
            else if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                leave = true;
            }
        }
        gui_synclist_scroll_stop(&list);
        viewportmanager_theme_undo(SCREEN_MAIN, false);
next_iter:
        if (leave)
            break;
    }

finish:
    ui_scene_close_modal(&scene, ret == 0, "cal-day-pop");
    if (ctx.sub_font >= 0) font_unload(ctx.sub_font);
    if (head_font >= 0) font_unload(head_font);
    *d = ctx.shown;
    return ret;
}

/* ---- Month View ---- */

struct month_page_ctx {
    struct ical_index *idx;
    struct extras_date shown;
    uint32_t *band;
    int font;
    int left, top, width;
};

static void month_title(const struct extras_date *d, char *buf, size_t size)
{
    format_date_lang(buf, size, LANG_EXTRAS_CALENDARS_MONTH_TITLE,
                     str(LANG_MONTH_JANUARY + d->month - 1), d->day, d->year);
}

static void fill_band_rect(struct stock_canvas *band, int rx, int ry,
                           int rw, int rh, uint32_t rgb)
{
    int y0 = MAX(ry, band->y);
    int y1 = MIN(ry + rh, band->y + band->height);
    int x0 = MAX(rx, band->x);
    int x1 = MIN(rx + rw, band->x + band->width);
    for (int y = y0; y < y1; y++)
    {
        uint32_t *row = band->pixels + (y - band->y) * band->width;
        for (int x = x0; x < x1; x++)
            row[x - band->x] = rgb;
    }
}

static void month_compose(struct stock_canvas *band, struct month_page_ctx *p)
{
    bool stock = is_stock_look();
    uint32_t bg = stock ? WHITE : theme_rgb24(UI_COLOR_BACKGROUND);
    uint32_t fg = stock ? BLACK : theme_rgb24(UI_COLOR_FOREGROUND);
    uint32_t muted = stock ? GREY : theme_rgb24(UI_COLOR_MUTED);
    uint32_t sel_bg = stock ? 0x0078d4 : 0x2458a8;
    uint32_t sel_fg = WHITE;
    uint32_t cell_in = 0x1a1d24;
    uint32_t cell_out = 0x0e1015;
    uint32_t accent = 0x52a8ff;

    for (int i = 0; i < band->width * band->height; i++)
        band->pixels[i] = bg;

    int head_base = stock ? HEAD_BASE : p->top + 13;
    int cell_x = stock ? CELL_X : p->left + (p->width - 7 * ((p->width) / 7)) / 2;
    int cell_dx = stock ? CELL_DX : p->width / 7;
    int cell_w = stock ? CELL_W : cell_dx - 3;
    int cell_y = stock ? CELL_Y : p->top + 18;
    int cell_dy = stock ? CELL_DY : (LCD_HEIGHT - cell_y - 2) / 6;
    int cell_h = stock ? CELL_H : cell_dy - 3;
    int flag_x = stock ? FLAG_X : cell_w - 11;

    struct extras_date first = { p->shown.year, p->shown.month, 1 };
    int lead = extras_week_day(&first);
    struct extras_date d = first;
    extras_step_days(&d, -(lead ? lead : 7));

    for (int col = 0; col < 7; col++)
    {
        const char *head = str(LANG_WEEKDAY_SUNDAY + col);
        int x = cell_x + col * cell_dx;
        int font = extras_font_for(p->font, head);
        put_text(band, font, head,
                 x + (cell_w - text_width(font, head)) / 2, head_base, muted);
    }

    for (int i = 0; i < 42; i++, extras_step_days(&d, 1))
    {
        int x = cell_x + (i % 7) * cell_dx;
        int y = cell_y + (i / 7) * cell_dy;
        if (y >= band->y + band->height || y + cell_dy <= band->y)
            continue;
        bool in = d.month == p->shown.month;
        bool selected = in && d.day == p->shown.day;
        if (stock)
        {
            stock_art_put(band, selected ? STOCK_ART_CALENDAR_HILITE_CELL :
                          in ? STOCK_ART_CALENDAR_ACTIVE_CELL :
                          STOCK_ART_CALENDAR_INACTIVE_CELL, x, y);
        }
        else
        {
            fill_band_rect(band, x, y, cell_w, cell_h,
                           selected ? sel_bg : in ? cell_in : cell_out);
        }
        char num[4];
        snprintf(num, sizeof(num), "%d", d.day);
        put_text(band, p->font, num, x + NUM_X, y + NUM_BASE,
                 selected ? sel_fg : in ? fg : muted);
        if (ical_has_event_on(p->idx, &d))
        {
            if (stock)
                stock_art_put(band, STOCK_ART_CALENDAR_FLAG,
                              x + FLAG_X, y + FLAG_Y);
            else
                fill_band_rect(band, x + flag_x, y + FLAG_Y + 2, 6, 6,
                               selected ? sel_fg : accent);
        }
    }
}

static void month_view_paint(const struct ui_view *view,
                             const struct ui_view_frame *frame)
{
    struct month_page_ctx *p = view->data;
    struct ui_rect clip = frame->clip;
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            p->band, clip.x, y, clip.width,
            MIN(BAND_ROWS, clip.y + clip.height - y),
        };
        month_compose(&band, p);
        stock_canvas_show(&band, clip);
    }
}

static const struct ui_view_ops month_view_ops = { .paint = month_view_paint };

static int month_page(struct ical_index *idx)
{
    char title[48];
    struct month_page_ctx p = { .idx = idx, .shown = extras_today() };
    struct viewport content;
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int handle = core_alloc(LCD_WIDTH * BAND_ROWS * sizeof(uint32_t));
    int ret = 0;
    bool select_wheeled = false;

    if (handle <= 0)
        return 0;
    p.band = core_get_data_pinned(handle);
    p.font = font_load_ex(CAL_FONT, 0, global_settings.glyphs_to_cache);
    if (p.font < 0)
        p.font = ui_theme_current_font_load(UI_FONT_LIST_VALUE,
                                            global_settings.glyphs_to_cache);

    month_title(&p.shown, title, sizeof(title));
    ui_scene_open_page(&scene, "calendars-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    p.left = content.x;
    p.top = content.y;
    p.width = content.width;
    struct ui_rect bounds = { content.x, content.y, content.width,
                              content.height };
    ui_view_init(&view, bounds, &month_view_ops, &p);
    ui_page_init(&page, &view);

    for (bool dirty = true; ; )
    {
        if (dirty)
        {
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            sb_update(SCREEN_MAIN, true);
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
            dirty = false;
        }

        int action = get_action(CONTEXT_LIST, HZ);
        int step = calendar_button_step(action);
        if (step)
        {
            extras_step_month(&p.shown, step);
            month_title(&p.shown, title, sizeof(title));
            dirty = true;
            continue;
        }
        if (!(button_status() & BUTTON_SELECT))
            select_wheeled = false;

        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }

        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
        {
            if (button_status() & BUTTON_SELECT)
            {
                select_wheeled = true;
                extras_step_month(&p.shown, 1);
            }
            else
                extras_step_days(&p.shown, 1);
            month_title(&p.shown, title, sizeof(title));
            dirty = true;
        }
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
        {
            if (button_status() & BUTTON_SELECT)
            {
                select_wheeled = true;
                extras_step_month(&p.shown, -1);
            }
            else
                extras_step_days(&p.shown, -1);
            month_title(&p.shown, title, sizeof(title));
            dirty = true;
        }
        else if (action == ACTION_STD_OK)
        {
            viewportmanager_theme_undo(SCREEN_MAIN, false);
            ret = day_page(idx, &p.shown);
            viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
            if (ret) break;
            ical_scan(idx, ICAL_DIR);
            month_title(&p.shown, title, sizeof(title));
            dirty = true;
        }
        else if (action == ACTION_STD_CONTEXT && !select_wheeled)
        {
            static const char *const month_menu[] = {
                "New Event", "Go to Today", "Next Month", "Previous Month"
            };
            char mtitle[32];
            format_short_date(mtitle, sizeof(mtitle), &p.shown);
            viewportmanager_theme_undo(SCREEN_MAIN, false);
            int choice = show_choice_list(mtitle, month_menu,
                                          ARRAYLEN(month_menu), 0);
            if (choice == -2)
            {
                viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
                ret = GO_TO_ROOT;
                break;
            }
            if (choice == 0)
            {
                struct ical_event ev;
                bool changed = false;
                ical_event_init_new(&ev, &p.shown);
                ret = run_event_editor(&ev, &changed);
                if (changed)
                    ical_scan(idx, ICAL_DIR);
            }
            else if (choice == 1)
                p.shown = extras_today();
            else if (choice == 2)
                extras_step_month(&p.shown, 1);
            else if (choice == 3)
                extras_step_month(&p.shown, -1);
            viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
            if (ret) break;
            month_title(&p.shown, title, sizeof(title));
            dirty = true;
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "calendars-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (p.font >= 0)
        font_unload(p.font);
    core_put_data_pinned(p.band);
    core_free(handle);
    return ret;
}

/* ---- To Do Editor ---- */

enum td_field {
    TDF_TITLE, TDF_DONE, TDF_DUE, TDF_PRIO, TDF_NOTES, TDF_SAVE, TDF_DELETE
};

struct td_editor_ctx {
    struct ical_todo *td;
    enum td_field rows[7];
    int row_count;
    bool dirty;
};

static const char *td_row_name(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct td_editor_ctx *ctx = data;
    switch (ctx->rows[i])
    {
        case TDF_TITLE:  return "Title";
        case TDF_DONE:   return "Completed";
        case TDF_DUE:    return "Due Date";
        case TDF_PRIO:   return "Priority";
        case TDF_NOTES:  return "Notes";
        case TDF_SAVE:   return str(LANG_SAVE);
        case TDF_DELETE: return "Delete To Do";
        default:         return "";
    }
}

static const char *td_row_value(int i, void *data, char *buf, size_t size)
{
    struct td_editor_ctx *ctx = data;
    const struct ical_todo *td = ctx->td;
    switch (ctx->rows[i])
    {
        case TDF_TITLE:
            return td->summary[0] ? td->summary : "Untitled";
        case TDF_DONE:
            return str(td->completed ? LANG_ON : LANG_OFF);
        case TDF_DUE:
            if (!td->has_due)
                return "None";
            format_short_date(buf, size, &td->due_date);
            return buf;
        case TDF_PRIO:
            return priority_labels[ical_priority_to_bucket(td->priority)];
        case TDF_NOTES:
            return td->notes[0] ? td->notes : "None";
        default:
            return NULL;
    }
}

static enum list_row_kind td_row_kind(int i, void *data)
{
    struct td_editor_ctx *ctx = data;
    switch (ctx->rows[i])
    {
        case TDF_DONE:   return LIST_ROW_TOGGLE;
        case TDF_SAVE:
        case TDF_DELETE: return LIST_ROW_ACTION;
        default:         return LIST_ROW_SETTING;
    }
}

static int run_todo_editor(struct ical_todo *td, bool *changed)
{
    struct td_editor_ctx ctx = { .td = td, .dirty = false };
    int n = 0;
    ctx.rows[n++] = TDF_TITLE;
    ctx.rows[n++] = TDF_DONE;
    ctx.rows[n++] = TDF_DUE;
    ctx.rows[n++] = TDF_PRIO;
    ctx.rows[n++] = TDF_NOTES;
    ctx.rows[n++] = TDF_SAVE;
    if (td->is_existing)
        ctx.rows[n++] = TDF_DELETE;
    ctx.row_count = n;

    struct gui_synclist list;
    struct ui_scene scene = {0};
    const char *title = td->is_existing ? "Edit To Do" : "New To Do";
    int sel = 0, action, ret = 0;

    ui_scene_open_page(&scene, "cal-todo-edit-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_init(&list, td_row_name, &ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, td_row_kind, td_row_value, NULL);
    gui_synclist_set_nb_items(&list, ctx.row_count);
    gui_synclist_select_item(&list, sel);
    gui_synclist_draw_settled(&list);

    for (;;)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        sel = gui_synclist_get_sel_pos(&list);

        if (action == ACTION_STD_OK)
        {
            enum td_field f = ctx.rows[sel];
            if (f == TDF_TITLE)
            {
                char tmp[sizeof(td->summary)];
                strlcpy(tmp, td->summary, sizeof(tmp));
                if (dialog_input(tmp, sizeof(tmp)) == 0)
                {
                    strlcpy(td->summary, tmp, sizeof(td->summary));
                    ctx.dirty = true;
                }
            }
            else if (f == TDF_DONE)
            {
                td->completed = !td->completed;
                ctx.dirty = true;
            }
            else if (f == TDF_DUE)
            {
                if (td->has_due)
                {
                    static const char *const opts[] = {
                        "Change Due Date", "Clear Due Date"
                    };
                    int ch = show_choice_list("Due Date", opts, 2, 0);
                    if (ch == -2) { ret = GO_TO_ROOT; break; }
                    if (ch == 0)
                    {
                        int r = extras_pick_date("Due Date", td->summary,
                                                 &td->due_date, 2000, 2099);
                        if (r < 0) { ret = GO_TO_ROOT; break; }
                        if (r > 0) ctx.dirty = true;
                    }
                    else if (ch == 1)
                    {
                        td->has_due = false;
                        ctx.dirty = true;
                    }
                }
                else
                {
                    td->due_date = extras_today();
                    int r = extras_pick_date("Due Date", td->summary,
                                             &td->due_date, 2000, 2099);
                    if (r < 0) { ret = GO_TO_ROOT; break; }
                    if (r > 0) { td->has_due = true; ctx.dirty = true; }
                }
            }
            else if (f == TDF_PRIO)
            {
                int cur = ical_priority_to_bucket(td->priority);
                int r = show_choice_list("Priority", priority_labels, 4, cur);
                if (r == -2) { ret = GO_TO_ROOT; break; }
                if (r >= 0)
                {
                    td->priority = ical_bucket_to_priority(r);
                    ctx.dirty = true;
                }
            }
            else if (f == TDF_NOTES)
            {
                char tmp[sizeof(td->notes)];
                strlcpy(tmp, td->notes, sizeof(tmp));
                if (dialog_input(tmp, sizeof(tmp)) == 0)
                {
                    strlcpy(td->notes, tmp, sizeof(td->notes));
                    ctx.dirty = true;
                }
            }
            else if (f == TDF_SAVE)
            {
                if (ical_save_todo(ICAL_DIR, td))
                {
                    *changed = true;
                    break;
                }
                splash(HZ, "Could not save To Do");
            }
            else if (f == TDF_DELETE)
            {
                int c = confirm_prompt("Delete this To Do?",
                                       LANG_DELETE, LANG_DIAG_CANCEL);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 1)
                {
                    if (ical_delete_component(td->path, td->comp_start,
                                              td->comp_end))
                    {
                        *changed = true;
                        break;
                    }
                    splash(HZ, "Could not delete To Do");
                }
            }
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            if (ctx.dirty)
            {
                int c = confirm_prompt("Save changes?",
                                       LANG_SAVE, LANG_EXTRAS_DISCARD);
                if (c < 0) { ret = GO_TO_ROOT; break; }
                if (c == 2) continue;
                if (c == 1)
                {
                    if (!ical_save_todo(ICAL_DIR, td))
                    {
                        splash(HZ, "Could not save To Do");
                        gui_synclist_draw(&list);
                        continue;
                    }
                    *changed = true;
                }
            }
            break;
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "cal-todo-edit-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/* ---- To Do's List ---- */

struct todo_list_ctx {
    struct ical_index *idx;
    int order[ICAL_MAX_TODOS];
    int count;
    int sub_font;
};

static const struct ical_index *todo_sort_idx;

static int cmp_todo_order(const void *a, const void *b)
{
    const struct ical_todo_idx *ta = &todo_sort_idx->todos[*(const int *)a];
    const struct ical_todo_idx *tb = &todo_sort_idx->todos[*(const int *)b];
    if (ta->completed != tb->completed)
        return ta->completed - tb->completed;
    int ba = ical_priority_to_bucket(ta->priority);
    int bb = ical_priority_to_bucket(tb->priority);
    if (ba != bb)
        return bb - ba; /* higher priority first */
    if (ta->has_due != tb->has_due)
        return tb->has_due - ta->has_due;
    if (ta->has_due)
    {
        int dc = extras_date_cmp(&ta->due_date, &tb->due_date);
        if (dc != 0)
            return dc;
    }
    return strcasecmp(ical_pool_str(todo_sort_idx, ta->summary_off),
                      ical_pool_str(todo_sort_idx, tb->summary_off));
}

static const char *todo_get_name(int row, void *data, char *buf, size_t size)
{
    struct todo_list_ctx *ctx = data;
    const struct ical_todo_idx *t = &ctx->idx->todos[ctx->order[row]];
    const char *s = ical_pool_str(ctx->idx, t->summary_off);
    strlcpy(buf, *s ? s : "Untitled", size);
    return buf;
}

static enum list_row_kind todo_get_kind(int row, void *data)
{
    (void)row; (void)data;
    return LIST_ROW_PLAIN;
}

static void draw_todo_row(struct list_putlineinfo_t *info)
{
    struct todo_list_ctx *ctx = info->list->data;
    const struct ical_todo_idx *t = &ctx->idx->todos[ctx->order[info->line]];
    struct viewport *vp = info->vp;
    const struct viewport *parent = info->list->parent[SCREEN_MAIN];
    int width = parent->x + parent->width - (vp->x + info->x);
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    bool stock = is_stock_look();

    if (info->is_selected)
    {
        const uint32_t *bar = stock ? ui_art_selection(TALL_ROW_H) : NULL;
        if (bar)
        {
            for (int y = 0; y < TALL_ROW_H; y++)
            {
                vp->fg_pattern = LCD_RGBPACK(bar[y] >> 16, (bar[y] >> 8) & 255,
                                             bar[y] & 255);
                lcd_hline(info->x, info->x + width - 1, info->y + y);
            }
        }
        else
        {
            vp->fg_pattern = LCD_RGBPACK(30, 34, 44);
            lcd_fillrect(info->x, info->y, width, TALL_ROW_H);
            vp->fg_pattern = ui_theme_rgb565(UI_COLOR_SELECTION);
            lcd_fillrect(info->x, info->y + 4, 4, TALL_ROW_H - 8);
        }
    }

    /* Checkbox at left of row. */
    int bx = info->x + 10, by = info->y + 14, bs = 16;
    unsigned box_col = info->is_selected ?
        (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
        (stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED));
    vp->fg_pattern = box_col;
    lcd_drawrect(bx, by, bs, bs);
    if (t->completed)
    {
        vp->fg_pattern = info->is_selected ?
            (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
            (stock ? LCD_RGBPACK(24, 130, 56) : ui_theme_rgb565(UI_COLOR_ACCENT));
        lcd_drawline(bx + 3, by + 8, bx + 6, by + 12);
        lcd_drawline(bx + 6, by + 12, bx + 13, by + 4);
        lcd_drawline(bx + 3, by + 9, bx + 6, by + 13);
        lcd_drawline(bx + 6, by + 13, bx + 13, by + 5);
    }

    int tx = info->x + 34;
    vp->fg_pattern = info->is_selected ?
        (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
        t->completed ? (stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED)) :
                       (stock ? fg : ui_theme_rgb565(UI_COLOR_FOREGROUND));
    lcd_putsxy(tx, info->y + TALL_TITLE_BASE - font_get(font)->ascent,
               (const unsigned char *)info->dsp_text);

    if (ctx->sub_font >= 0)
    {
        char sub[80];
        if (t->completed)
            strlcpy(sub, "Completed", sizeof(sub));
        else if (t->has_due)
        {
            char dbuf[24];
            format_short_date(dbuf, sizeof(dbuf), &t->due_date);
            snprintf(sub, sizeof(sub), "Due %s", dbuf);
        }
        else
            strlcpy(sub, "No due date", sizeof(sub));

        int b = ical_priority_to_bucket(t->priority);
        if (b > 0)
        {
            static const char *const pb[4] = { "", "  ! Low", "  !! Med",
                                               "  !!! High" };
            size_t len = strlen(sub);
            snprintf(sub + len, sizeof(sub) - len, "%s", pb[b]);
        }

        vp->font = ctx->sub_font;
        vp->fg_pattern = info->is_selected ?
            (stock ? fg : ui_theme_rgb565(UI_COLOR_SELECTED_TEXT)) :
            (stock ? SUB_RGB : ui_theme_rgb565(UI_COLOR_MUTED));
        lcd_putsxy(tx, info->y + TALL_SUB_BASE - font_get(ctx->sub_font)->ascent,
                   (const unsigned char *)sub);
        vp->font = font;
    }

    if (!info->is_selected)
    {
        vp->fg_pattern = stock ? ROW_RULE_RGB :
                                 ui_theme_rgb565(UI_COLOR_SEPARATOR);
        lcd_hline(info->x, info->x + width - 1, info->y + TALL_ROW_H - 1);
    }
    vp->fg_pattern = fg;
}

static int todo_page(struct ical_index *idx)
{
    struct todo_list_ctx ctx = { .idx = idx };
    struct ui_scene scene = {0};
    int ret = 0, sel = 0;

    ctx.sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                              global_settings.glyphs_to_cache);
    int head_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                               global_settings.glyphs_to_cache);
    ui_scene_open_page(&scene, "cal-todo-push");

    for (;;)
    {
        ctx.count = idx->todo_count;
        for (int i = 0; i < ctx.count; i++)
            ctx.order[i] = i;
        todo_sort_idx = idx;
        if (ctx.count > 1)
            qsort(ctx.order, (size_t)ctx.count, sizeof(ctx.order[0]),
                  cmp_todo_order);

        const char *title = str(LANG_EXTRAS_CALENDARS_TODOS);
        sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);

        if (ctx.count == 0)
        {
            paint_empty_page("No To Do's", "SELECT: New To Do",
                             head_font, ctx.sub_font);
            int action = get_action(CONTEXT_LIST, HZ);
            if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
                break;
            if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                break;
            }
            if (action == ACTION_STD_OK || action == ACTION_STD_CONTEXT)
            {
                struct ical_todo td;
                bool changed = false;
                ical_todo_init_new(&td);
                ret = run_todo_editor(&td, &changed);
                if (ret) break;
                if (changed) ical_scan(idx, ICAL_DIR);
            }
            continue;
        }

        struct gui_synclist list;
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
        gui_synclist_init(&list, todo_get_name, &ctx, false, 1, NULL);
        gui_synclist_set_title(&list, title, Icon_NOICON);
        gui_synclist_set_row_callbacks(&list, todo_get_kind, NULL, NULL);
        gui_synclist_set_row_height(&list, TALL_ROW_H);
        list.callback_draw_item = draw_todo_row;
        gui_synclist_set_nb_items(&list, ctx.count);
        gui_synclist_select_item(&list, MIN(sel, ctx.count - 1));
        gui_synclist_draw_settled(&list);

        bool reload = false, leave = false;
        while (!reload && !leave)
        {
            int action;
            list_do_action(CONTEXT_TREE, HZ, &list, &action);
            sel = gui_synclist_get_sel_pos(&list);

            if (action == ACTION_STD_OK)
            {
                /* SELECT toggles completed in place and writes atomically. */
                const struct ical_todo_idx *ti = &idx->todos[ctx.order[sel]];
                char path[MAX_PATH];
                ical_file_path(idx, ti->file_idx, path, sizeof(path));
                struct ical_todo td;
                if (ical_load_todo(path, ti->comp_start, ti->comp_end, &td))
                {
                    td.completed = !td.completed;
                    if (ical_save_todo(ICAL_DIR, &td))
                    {
                        ical_scan(idx, ICAL_DIR);
                        reload = true;
                    }
                    else
                        splash(HZ, "Could not save To Do");
                }
            }
            else if (action == ACTION_STD_CONTEXT)
            {
                static const char *const menu_todo[] = {
                    "New To Do", "Edit To Do", "Delete To Do"
                };
                gui_synclist_scroll_stop(&list);
                viewportmanager_theme_undo(SCREEN_MAIN, false);
                int choice = show_choice_list(title, menu_todo,
                                              ARRAYLEN(menu_todo), 0);
                if (choice == -2) { ret = GO_TO_ROOT; goto finish; }
                if (choice == 0)
                {
                    struct ical_todo td;
                    bool changed = false;
                    ical_todo_init_new(&td);
                    ret = run_todo_editor(&td, &changed);
                    if (ret) goto finish;
                    if (changed) ical_scan(idx, ICAL_DIR);
                }
                else if (choice == 1)
                {
                    const struct ical_todo_idx *ti =
                        &idx->todos[ctx.order[sel]];
                    char path[MAX_PATH];
                    ical_file_path(idx, ti->file_idx, path, sizeof(path));
                    struct ical_todo td;
                    if (ical_load_todo(path, ti->comp_start, ti->comp_end, &td))
                    {
                        bool changed = false;
                        ret = run_todo_editor(&td, &changed);
                        if (ret) goto finish;
                        if (changed) ical_scan(idx, ICAL_DIR);
                    }
                }
                else if (choice == 2)
                {
                    int c = confirm_prompt("Delete this To Do?",
                                           LANG_DELETE, LANG_DIAG_CANCEL);
                    if (c < 0) { ret = GO_TO_ROOT; goto finish; }
                    if (c == 1)
                    {
                        const struct ical_todo_idx *ti =
                            &idx->todos[ctx.order[sel]];
                        char path[MAX_PATH];
                        ical_file_path(idx, ti->file_idx, path, sizeof(path));
                        if (ical_delete_component(path, ti->comp_start,
                                                  ti->comp_end))
                            ical_scan(idx, ICAL_DIR);
                    }
                }
                reload = true;
                goto next_todo_iter;
            }
            else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            {
                leave = true;
            }
            else if (default_event_handler(action) == SYS_USB_CONNECTED)
            {
                ret = GO_TO_ROOT;
                leave = true;
            }
        }
        gui_synclist_scroll_stop(&list);
        viewportmanager_theme_undo(SCREEN_MAIN, false);
next_todo_iter:
        if (leave)
            break;
    }

finish:
    ui_scene_close_modal(&scene, ret == 0, "cal-todo-pop");
    if (ctx.sub_font >= 0) font_unload(ctx.sub_font);
    if (head_font >= 0) font_unload(head_font);
    return ret;
}

/* ---- Top-level Calendars menu ---- */

enum { ROW_ALL, ROW_TODOS, ROW_ALARMS, ROW_COUNT };
static int alarm_sound;

static const char *row_name(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    static const int name[ROW_COUNT] = {
        LANG_EXTRAS_CALENDARS_ALL, LANG_EXTRAS_CALENDARS_TODOS,
        LANG_EXTRAS_CALENDARS_ALARMS,
    };
    return str(name[i]);
}

static const char *row_value(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    static const int sound[3] = {
        LANG_EXTRAS_CALENDARS_BEEP, LANG_EXTRAS_CALENDARS_NONE, LANG_OFF,
    };
    return i == ROW_ALARMS ? str(sound[alarm_sound]) : NULL;
}

static enum list_row_kind row_kind(int i, void *data)
{
    (void)data;
    return i == ROW_ALARMS ? LIST_ROW_SETTING : LIST_ROW_MENU;
}

static enum list_pane_kind row_pane(int i, void *data, int *art)
{
    (void)i; (void)data; (void)art;
    struct menu_pane_content pane;
    extras_calendars_pane(&pane);
    menu_pane_set_picture(&pane);
    return LIST_PANE_PICTURE;
}

int extras_calendars(void)
{
    static int selected;
    struct gui_synclist list;
    struct menu_pane_content pane;
    struct ui_scene scene = {0};
    int action, ret = 0;

    int handle = core_alloc_free_space(sizeof(struct ical_index));
    if (handle <= 0)
        return 0;
    struct ical_index *idx = core_get_data_pinned(handle);
    ical_scan(idx, ICAL_DIR);

    ui_scene_open_page(&scene, "calendars-list-push");
    gui_synclist_init(&list, row_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, str(LANG_EXTRAS_CALENDARS), Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, row_kind, row_value, NULL);
    gui_synclist_set_pane_callback(&list, row_pane);
    gui_synclist_set_nb_items(&list, ROW_COUNT);
    gui_synclist_select_item(&list, selected);
    sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_draw_settled(&list);
    extras_calendars_pane(&pane);
    int stamp = pane.stamp;

    while (!ret)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        selected = gui_synclist_get_sel_pos(&list);
        if (action == ACTION_STD_OK)
        {
            if (selected == ROW_ALARMS)
                alarm_sound = (alarm_sound + 1) % 3;
            else
                ret = selected == ROW_ALL ? month_page(idx) : todo_page(idx);
            sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS), Icon_NOICON,
                                    SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
            ret = GO_TO_ROOT;
        extras_calendars_pane(&pane);
        if (pane.stamp != stamp)
        {
            stamp = pane.stamp;
            gui_synclist_draw(&list);
        }
    }
    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "calendars-list-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    core_put_data_pinned(idx);
    core_free(handle);
    return ret;
}
