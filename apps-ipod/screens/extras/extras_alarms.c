/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Alarms as the stock iPod Classic 1.1.2 draws it. The Alarms list
 * (Create Alarm, Sleep Timer, then one row per alarm) and every list under
 * it share the split pane's red alarm clock; the Date and Time editors are
 * full pages on the black status bar.
 *
 * Sleep Timer is the player's own (set_sleeptimer_duration). The alarms are
 * alarm_table, which extras_alarm_ring.c keeps in its file and rings.
 *
 * Parts: the alarms; the text of a row and of the pane; the
 * list pages; the Date and Time editors; the pages themselves.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "fixedpoint.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "input/action.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "ui/theme_art.h"
#include "widgets/list.h"
#include "root_menu.h"
#include "extras.h"
#include "ui/scene.h"
#include "extras_alarm_ring.h"
#include "extras_bar.h"
#include "extras_font.h"
#include "extras_calendars.h"

/* Where stock draws the pane's clock and its two lines of text. */
#define PANE_ART_X 178
#define PANE_ART_Y 56
#define PANE_BASELINE 205
#define PANE_TEXT_X (MENU_PANE_X + (LCD_WIDTH - MENU_PANE_X) / 2)
/* The Alarms list's two-line rows. */
#define TALL_ROW 44
#define TALL_TEXT_X 5
#define TALL_SUB_BASELINE 35
#define TALL_TITLE_BASELINE 18
#define CHEVRON_X 144
#define CHEVRON_Y 15
#define SUBTITLE_RGB LCD_RGBPACK(107, 109, 107)
#define SEPARATOR_RGB LCD_RGBPACK(222, 223, 222)
/* The editors. */
#define DIGIT_FONT FONT_DIR "/21-helvetica-bold.pbf"
/* The pane's and the editors' two lines. */
#define CAPTION_FONT FONT_DIR "/17-helvetica-bold.pbf"
#define CLOCK_X 14
#define CLOCK_Y 52
#define CLOCK_CX 80
#define CLOCK_CY 118
#define CALENDAR_X 16
#define CALENDAR_Y 56
#define BAR_X 150
#define BAR_Y 97
#define WELL_Y 101
#define WELL_W 40
#define SELECTION_Y 103
#define DIGIT_BASELINE 129
#define CAPTION_X 80
#define CAPTION_BASELINE 205
#define CAPTION_LEADING 21
#define BAND_ROWS EXTRAS_BAND_ROWS
#define WHITE 0xffffff

/* What a page returns besides a chosen row: back to the page under it, stay
 * on it, or leave Alarms for the root menu (a USB attach). */
enum { BACK = -100, STAY = -101, LEFT = -102 };

static int editing;                     /* the alarm the editor shows */
static int sub_font = -1, digit_font = -1, caption_font = -1;

static const int sleep_minutes[] = { 0, 15, 30, 60, 90, 120 };
static const int repeat_names[] = {
    LANG_EXTRAS_ALARMS_ONCE, LANG_EXTRAS_ALARMS_EVERY_DAY,
    LANG_EXTRAS_ALARMS_WEEKDAYS, LANG_EXTRAS_ALARMS_WEEKENDS,
    LANG_EXTRAS_ALARMS_EVERY_WEEK, LANG_EXTRAS_ALARMS_EVERY_MONTH,
    LANG_EXTRAS_ALARMS_EVERY_YEAR,
};
#define LABEL_ALARM 3
static const int sound_names[] = {
    LANG_EXTRAS_ALARMS_NONE, LANG_EXTRAS_ALARMS_BEEP,
};
#define SOUND_BEEP 1
static const int editor_rows[] = {
    LANG_EXTRAS_ALARMS_ALARM, LANG_DATE, LANG_TIME, LANG_REPEAT,
    LANG_SOUND_PROPERTIES, LANG_EXTRAS_ALARMS_LABEL, LANG_DELETE,
};
enum { ROW_ON, ROW_DATE, ROW_TIME, ROW_REPEAT, ROW_SOUND, ROW_LABEL,
       ROW_DELETE };
static const int month_names[] = {
    LANG_MONTH_JANUARY, LANG_MONTH_FEBRUARY, LANG_MONTH_MARCH,
    LANG_MONTH_APRIL, LANG_MONTH_MAY, LANG_MONTH_JUNE, LANG_MONTH_JULY,
    LANG_MONTH_AUGUST, LANG_MONTH_SEPTEMBER, LANG_MONTH_OCTOBER,
    LANG_MONTH_NOVEMBER, LANG_MONTH_DECEMBER,
};

/* ---- the alarms ---- */

/* A new alarm, on, at the current minute, today. */
static bool create_alarm(void)
{
    if (alarm_table.count >= MAX_ALARMS)
        return false;
    struct tm *now = get_time();
    alarm_table.list[alarm_table.count] = (struct alarm){
        .on = 1, .sound = SOUND_BEEP, .label = LABEL_ALARM,
        .hour = now->tm_hour, .minute = now->tm_min,
        .month = now->tm_mon + 1, .day = now->tm_mday,
        .year = now->tm_year + 1900,
    };
    editing = alarm_table.count++;
    alarm_table_save();
    return true;
}

static void delete_alarm(int index)
{
    alarm_table.count--;
    memmove(&alarm_table.list[index], &alarm_table.list[index + 1],
            (alarm_table.count - index) * sizeof(alarm_table.list[0]));
    alarm_table_save();
}

static int days_in_month(int month, int year)
{
    static const uint8_t days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
                                    30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return days[month - 1] + (month == 2 && leap);
}

/* ---- the text of a row and of the pane ---- */

static bool clock_24h(void)
{
    return global_settings.timeformat == 0;
}

static char *format_time(char *buffer, size_t size, int hour, int minute)
{
    if (clock_24h())
        snprintf(buffer, size, "%d:%02d", hour, minute);
    else
        snprintf(buffer, size, str(hour < 12 ? LANG_EXTRAS_ALARMS_TIME_AM :
                                               LANG_EXTRAS_ALARMS_TIME_PM),
                 (hour + 11) % 12 + 1, minute);
    return buffer;
}

/* "26 Nov 7:07 PM" */
static char *format_when(char *buffer, size_t size, const struct alarm *a)
{
    char time[24];
    format_time(time, sizeof(time), a->hour, a->minute);
    snprintf(buffer, size, str(LANG_EXTRAS_ALARMS_WHEN), a->day,
             str(month_names[a->month - 1]), time);
    return buffer;
}

static const char *sleep_caption(char *buffer, size_t size)
{
    if (!get_sleep_timer_active())
        return str(LANG_EXTRAS_ALARMS_INACTIVE);
    snprintf(buffer, size, str(LANG_EXTRAS_ALARMS_MINUTES),
             get_sleep_timer_duration() / 60);
    return buffer;
}

static const char *sleep_remaining(char *buffer, size_t size)
{
    if (!get_sleep_timer_active())
        return str(LANG_EXTRAS_ALARMS_INACTIVE);
    snprintf(buffer, size, str(LANG_EXTRAS_ALARMS_REMAINING),
             (get_sleep_timer() + 59) / 60);
    return buffer;
}

/* ---- the list pages ---- */

enum pane_text { PANE_MAIN, PANE_ALARM, PANE_SLEEP };

struct page;
/* What choosing `row` does: STAY, or what the page returns. */
typedef int page_choose(struct page *page, int row);

struct page {
    int title;                  /* a LANG_ id */
    int count, selection;
    list_get_name *name;
    list_get_kind *kind;
    list_get_name *value;
    page_choose *choose;
    enum pane_text pane;
    bool tall;
};

static enum pane_text pane_text;

/* The clock, under it the sleep timer's state, an alarm's time and label,
 * or for Create Alarm nothing. */
static void centred_text(struct stock_canvas *band, int font,
                         const char *text, int cx, int baseline)
{
    if (font < 0 || !*text)
        return;
    int width;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    stock_canvas_text(band, font, text, cx - width / 2,
                      baseline - font_get(font)->ascent, WHITE);
}

/* The pane's two lines, in CAPTION_FONT rather than menu_pane's title
 * font; every Alarms pane shows the same clock, so the latest text is the
 * one to draw. */
static char pane_title[48], pane_caption[48];

static void paint_pane_text(struct stock_canvas *band)
{
    centred_text(band, caption_font, pane_title, PANE_TEXT_X, PANE_BASELINE);
    centred_text(band, caption_font, pane_caption, PANE_TEXT_X,
                 PANE_BASELINE + CAPTION_LEADING);
}

static enum list_pane_kind get_pane(int item, void *data, int *art)
{
    (void)data;
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = STOCK_ART_ALARMS_CLOCK_STATIC,
        .x = PANE_ART_X, .y = PANE_ART_Y, .paint = paint_pane_text,
    };
    /* Create Alarm shows the first alarm, as stock does once one exists. */
    int index = pane_text == PANE_ALARM ? editing :
                pane_text != PANE_MAIN ? -1 : item == 0 ? 0 : item - 2;
    pane_title[0] = pane_caption[0] = '\0';
    if (pane_text == PANE_SLEEP || (pane_text == PANE_MAIN && item == 1))
    {
        char buffer[sizeof(pane_title)];
        strlcpy(pane_title, sleep_caption(buffer, sizeof(buffer)),
                sizeof(pane_title));
    }
    else if (index >= 0 && index < (int)alarm_table.count)
    {
        const struct alarm *a = &alarm_table.list[index];
        format_time(pane_title, sizeof(pane_title), a->hour, a->minute);
        strlcpy(pane_caption, str(alarm_label_names[a->label]),
                sizeof(pane_caption));
    }
    /* A new text repaints the pane. */
    unsigned stamp = 0;
    for (const char *t = pane_title; *t; t++)
        stamp = stamp * 31 + (unsigned char)*t;
    for (const char *t = pane_caption; *t; t++)
        stamp = stamp * 31 + (unsigned char)*t;
    c.stamp = (int)(stamp & 0x7fffffff);
    menu_pane_set_picture(&c);
    *art = c.art;
    return LIST_PANE_PICTURE;
}

/* The Alarms list's rows: the name over a smaller grey line. */
static void tall_texts(int item, const char **title, const char **sub,
                       char *buffer, size_t size)
{
    if (item == 0)
    {
        *title = str(LANG_EXTRAS_ALARMS_CREATE);
        *sub = str(LANG_EXTRAS_ALARMS_NEW);
    }
    else if (item == 1)
    {
        *title = str(LANG_SLEEP_TIMER);
        *sub = sleep_remaining(buffer, size);
    }
    else
    {
        const struct alarm *a = &alarm_table.list[item - 2];
        *title = str(alarm_label_names[a->label]);
        *sub = format_when(buffer, size, a);
    }
}

static void draw_tall_row(struct list_putlineinfo_t *info)
{
    struct viewport *vp = info->vp;
    char buffer[48];
    const char *title, *sub;
    tall_texts(info->line, &title, &sub, buffer, sizeof(buffer));
    unsigned fg = vp->fg_pattern;
    int font = vp->font;
    int x = info->x + TALL_TEXT_X;
    /* An owner-drawn row lays its own bar: stock's 44-row one. */
    const uint32_t *bar = ui_art_selection(TALL_ROW);
    for (int row = 0; info->is_selected && bar && row < TALL_ROW; row++)
    {
        vp->fg_pattern = LCD_RGBPACK(bar[row] >> 16, (bar[row] >> 8) & 255,
                                     bar[row] & 255);
        lcd_hline(info->x, info->x + MENU_PANE_X - 1, info->y + row);
    }
    vp->fg_pattern = fg;
    lcd_putsxy(x, info->y + TALL_TITLE_BASELINE - font_get(font)->ascent,
               (const unsigned char *)title);
    if (sub_font >= 0)
    {
        vp->font = sub_font;
        vp->fg_pattern = info->is_selected ? fg : SUBTITLE_RGB;
        lcd_putsxy(x, info->y + TALL_SUB_BASELINE -
                   font_get(sub_font)->ascent, (const unsigned char *)sub);
        vp->font = font;
    }
    if (info->is_selected)
        stock_art_draw(STOCK_ART_SYSTEM_SUBMENU, vp->x + info->x + CHEVRON_X,
                       vp->y + info->y + CHEVRON_Y,
                       (struct ui_rect){ vp->x, vp->y, vp->width,
                                         vp->height });
    else
    {
        vp->fg_pattern = SEPARATOR_RGB;
        lcd_hline(info->x, info->x + MENU_PANE_X - 1, info->y + TALL_ROW - 1);
    }
    vp->fg_pattern = fg;
}

static void open_page_chrome(const struct page *page)
{
    pane_text = page->pane;
    sb_set_persistent_title(str(page->title), Icon_NOICON, SCREEN_MAIN);
}

static void init_list(struct gui_synclist *list, struct page *page)
{
    gui_synclist_init(list, page->name, page, false, 1, NULL);
    gui_synclist_set_title(list, str(page->title), Icon_NOICON);
    gui_synclist_set_row_callbacks(list, page->kind, page->value, NULL);
    gui_synclist_set_pane_callback(list, get_pane);
    if (page->tall)
    {
        gui_synclist_set_row_height(list, TALL_ROW);
        list->callback_draw_item = draw_tall_row;
    }
    gui_synclist_set_nb_items(list, page->count);
    gui_synclist_select_item(list, MIN(page->selection, page->count - 1));
}

/* Run `page` until it is left: BACK, LEFT, or what a choice returned. */
static int show_page(struct page *page)
{
    struct gui_synclist list;
    struct ui_scene scene = {0};
    int action, result = BACK;
    long refreshed = current_tick;
    ui_scene_open_page(&scene, "alarms-list-push");
    open_page_chrome(page);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    init_list(&list, page);
    gui_synclist_draw_settled(&list);
    while (true)
    {
        list_do_action(CONTEXT_TREE, HZ, &list, &action);
        if (action == ACTION_STD_OK)
        {
            page->selection = gui_synclist_get_sel_pos(&list);
            int done = page->choose(page, page->selection);
            if (done != STAY)
            {
                result = done;
                break;
            }
            open_page_chrome(page);
            init_list(&list, page);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        else if (action == ACTION_REDRAW ||
                 (page->tall && get_sleep_timer_active() &&
                  TIME_AFTER(current_tick, refreshed + 10 * HZ)))
        {
            refreshed = current_tick;
            gui_synclist_draw(&list);
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = LEFT;
            break;
        }
    }
    page->selection = gui_synclist_get_sel_pos(&list);
    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, result != LEFT, "alarms-list-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return result;
}

static enum list_row_kind kind_menu(int item, void *data)
{
    (void)item;
    (void)data;
    return LIST_ROW_MENU;
}

static enum list_row_kind kind_option(int item, void *data)
{
    (void)item;
    (void)data;
    return LIST_ROW_OPTION;
}

static const char *name_lang(const int *ids, int item)
{
    return str(ids[item]);
}

/* A list of choices leaves with the row chosen. */
static int choose_row(struct page *page, int row)
{
    (void)page;
    return row;
}

/* Choose `*value` from `count` rows, starting on row `start`; BACK or LEFT
 * unless one is chosen. */
static int show_choice(int title, list_get_name *name, int count, int start,
                       uint8_t *value)
{
    struct page page = {
        .title = title, .count = count, .selection = start, .name = name,
        .kind = kind_option, .choose = choose_row, .pane = PANE_ALARM,
    };
    int result = show_page(&page);
    if (result >= 0)
    {
        *value = result;
        alarm_table_save();
    }
    return result;
}

static const char *repeat_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return name_lang(repeat_names, item);
}

static const char *label_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return name_lang(alarm_label_names, item);
}

static const char *sound_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return name_lang(sound_names, item);
}

/* Sounds > Playlists: no playlist can sound an alarm here. */
static const char *none_name(int item, void *data, char *buf, size_t size)
{
    (void)item; (void)data; (void)buf; (void)size;
    return str(LANG_EXTRAS_ALARMS_NONE);
}

/* A tone or playlist chosen goes back to the alarm. */
static int choose_sounds(struct page *page, int row)
{
    (void)page;
    struct alarm *a = &alarm_table.list[editing];
    uint8_t none = 0;
    int result = row == 0 ?
        show_choice(LANG_EXTRAS_ALARMS_SOUNDS, sound_name,
                    ARRAYLEN(sound_names), a->sound, &a->sound) :
        show_choice(LANG_EXTRAS_ALARMS_SOUNDS, none_name, 1, 0, &none);
    if (row != 0 && result >= 0)
    {
        a->sound = 0;
        alarm_table_save();
    }
    return result == LEFT ? LEFT : result >= 0 ? BACK : STAY;
}

static const char *sounds_row(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return str(item ? LANG_PLAYLISTS : LANG_EXTRAS_ALARMS_TONES);
}

static int show_sounds(void)
{
    struct page page = {
        .title = LANG_EXTRAS_ALARMS_SOUNDS, .count = 2, .name = sounds_row,
        .kind = kind_menu, .choose = choose_sounds, .pane = PANE_ALARM,
    };
    return show_page(&page);
}

/* ---- the Date and Time editors ---- */

struct editor {
    bool date;
    int field;                  /* the slot the wheel changes */
    int value[3];
};

/* Stock's picker: the bar, a well for each field, and the selection over
 * the field being set. */
static const int well_x[3] = { 165, 213, 261 };

static void put_picker(struct stock_canvas *band, int fields, int field)
{
    stock_art_put(band, STOCK_ART_DATETIMEPICKER_BAR, BAR_X, BAR_Y);
    for (int i = 0; i < fields; i++)
        stock_art_put(band, STOCK_ART_DATETIMEPICKER_WELL, well_x[i], WELL_Y);
    stock_art_put(band, STOCK_ART_DATETIMEPICKER_SELECTION,
                  well_x[field] + 1, SELECTION_Y);
}

/* A tapered hand from the centre, `length` long at `degrees` clockwise
 * from 12, `root` and `tip` its half-widths in 1/16 px. */
static void put_hand(struct stock_canvas *band, int degrees, int length,
                     int root, int tip)
{
    long s = fp14_sin(degrees), c = fp14_cos(degrees);
    int reach = length + 3;
    for (int y = MAX(band->y, CLOCK_CY - reach);
         y < MIN(band->y + band->height, CLOCK_CY + reach + 1); y++)
        for (int x = CLOCK_CX - reach; x <= CLOCK_CX + reach; x++)
        {
            unsigned hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                {
                    /* The sample, in 1/16 px from the centre. */
                    long dx = (x - CLOCK_CX) * 16 + sx * 4 - 6;
                    long dy = (y - CLOCK_CY) * 16 + sy * 4 - 6;
                    long along = (dx * s - dy * c) >> 14;
                    long across = (dx * c + dy * s) >> 14;
                    if (along < 0 || along > length * 16)
                        continue;
                    long half = root + (tip - root) * along / (length * 16);
                    hits += across <= half && across >= -half;
                }
            if (hits)
                stock_canvas_blend(band, x, y, 0, hits * 255 / 16);
        }
}

static void put_cap(struct stock_canvas *band)
{
    for (int y = CLOCK_CY - 4; y <= CLOCK_CY + 4; y++)
        for (int x = CLOCK_CX - 4; x <= CLOCK_CX + 4; x++)
        {
            int d = (x - CLOCK_CX) * (x - CLOCK_CX) +
                    (y - CLOCK_CY) * (y - CLOCK_CY);
            if (d <= 16)
                stock_canvas_blend(band, x, y, d <= 4 ? WHITE : 0, 255);
        }
}

static void slot_text(const struct editor *e, int slot, char *buf,
                      size_t size)
{
    int v = e->value[slot];
    if (e->date)
        snprintf(buf, size, "%d", slot == 2 ? v % 100 : v);
    else if (slot == 0)
        snprintf(buf, size, "%d", clock_24h() ? v : (v + 11) % 12 + 1);
    else if (slot == 1)
        snprintf(buf, size, "%02d", v);
    else
        strlcpy(buf, str(v ? LANG_EXTRAS_ALARMS_PM : LANG_EXTRAS_ALARMS_AM),
                size);
}

static void compose_editor(struct stock_canvas *band, const struct editor *e)
{
    const struct alarm *a = &alarm_table.list[editing];
    stock_art_put(band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    extras_bar_put(band, str(LANG_EXTRAS_ALARMS));
    if (e->date)
        extras_calendars_icon_put(band, CALENDAR_X, CALENDAR_Y, a->month,
                                  a->day);
    else
    {
        stock_art_put(band, STOCK_ART_CLOCK_LARGE, CLOCK_X, CLOCK_Y);
        put_hand(band, a->hour % 12 * 30 + a->minute / 2, 18, 40, 16);
        put_hand(band, a->minute * 6, 29, 32, 8);
        put_cap(band);
    }
    int slots = e->date || !clock_24h() ? 3 : 2;
    put_picker(band, slots, e->field);
    for (int i = 0; i < slots; i++)
    {
        char text[16];
        slot_text(e, i, text, sizeof(text));
        centred_text(band, extras_font_for(digit_font, text), text,
                     well_x[i] + WELL_W / 2, DIGIT_BASELINE);
    }
    char time[24];
    format_time(time, sizeof(time), a->hour, a->minute);
    centred_text(band, caption_font, time, CAPTION_X, CAPTION_BASELINE);
    centred_text(band, caption_font, str(alarm_label_names[a->label]),
                 CAPTION_X, CAPTION_BASELINE + CAPTION_LEADING);
}

static void paint_editor(const struct editor *e)
{
    uint32_t *pixels = extras_band(0);
    if (!pixels)
        return;
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS)
    {
        struct stock_canvas band = { pixels, 0, y, LCD_WIDTH, BAND_ROWS };
        compose_editor(&band, e);
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   BAND_ROWS });
    }
    lcd_update();
}

/* Write the editor's fields back to the alarm being edited. */
static void store_editor(const struct editor *e)
{
    struct alarm *a = &alarm_table.list[editing];
    if (e->date)
    {
        a->month = e->value[0];
        a->day = e->value[1];
        a->year = e->value[2];
    }
    else
    {
        a->hour = clock_24h() ? e->value[0] :
                  e->value[0] % 12 + (e->value[2] ? 12 : 0);
        a->minute = e->value[1];
    }
}

static void step_field(struct editor *e, int delta)
{
    int *v = &e->value[e->field];
    if (e->date)
    {
        if (e->field == 0)
            *v = (*v + 11 + delta) % 12 + 1;
        else if (e->field == 1)
        {
            int days = days_in_month(e->value[0], e->value[2]);
            *v = (*v + days - 1 + delta) % days + 1;
        }
        else
            *v = MIN(MAX(*v + delta, 2000), 2099);
        e->value[1] = MIN(e->value[1], days_in_month(e->value[0],
                                                     e->value[2]));
    }
    else if (e->field == 0)
        *v = (*v + (clock_24h() ? 24 : 12) + delta) % (clock_24h() ? 24 : 12);
    else if (e->field == 1)
        *v = (*v + 60 + delta) % 60;
    else
        *v = !*v;
    store_editor(e);
}

static int show_editor(bool date)
{
    const struct alarm *a = &alarm_table.list[editing];
    struct editor e = { .date = date };
    if (date)
    {
        e.value[0] = a->month;
        e.value[1] = a->day;
        e.value[2] = a->year;
    }
    else
    {
        e.value[0] = clock_24h() ? a->hour : a->hour % 12;
        e.value[1] = a->minute;
        e.value[2] = a->hour >= 12;
    }
    int fields = date || !clock_24h() ? 3 : 2;
    int result = BACK;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "alarms-editor-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    digit_font = font_load_ex(DIGIT_FONT, 0, global_settings.glyphs_to_cache);
    paint_editor(&e);
    while (true)
    {
        int action = get_action(CONTEXT_STD, HZ);
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ||
            action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
        {
            step_field(&e, action == ACTION_STD_NEXT ||
                           action == ACTION_STD_NEXTREPEAT ? 1 : -1);
            paint_editor(&e);
        }
        else if (action == ACTION_STD_OK)
        {
            if (++e.field >= fields)
                break;
            paint_editor(&e);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        else if (action == ACTION_NONE || action == ACTION_REDRAW)
            paint_editor(&e);
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = LEFT;
            break;
        }
    }
    alarm_table_save();
    ui_scene_close_modal(&scene, result != LEFT, "alarms-editor-pop");
    font_unload(digit_font);
    digit_font = -1;
    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return result;
}

/* ---- the pages themselves ---- */

static const char *editor_name(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return str(editor_rows[item]);
}

static enum list_row_kind editor_kind(int item, void *data)
{
    (void)data;
    return item == ROW_ON ? LIST_ROW_SETTING :
           item == ROW_DELETE ? LIST_ROW_ACTION : LIST_ROW_MENU;
}

static const char *editor_value(int item, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    if (item != ROW_ON)
        return NULL;
    return str(alarm_table.list[editing].on ? LANG_ON : LANG_OFF);
}

static int choose_editor_row(struct page *page, int row)
{
    (void)page;
    struct alarm *a = &alarm_table.list[editing];
    int result = STAY;
    switch (row)
    {
        case ROW_ON:
            a->on = !a->on;
            alarm_table_save();
            return STAY;
        case ROW_DATE:
        case ROW_TIME:
            result = show_editor(row == ROW_DATE);
            break;
        case ROW_REPEAT:
            result = show_choice(LANG_EXTRAS_ALARMS, repeat_name,
                                 ARRAYLEN(repeat_names), a->repeat,
                                 &a->repeat);
            break;
        case ROW_SOUND:
            result = show_sounds();
            break;
        case ROW_LABEL:
            /* Stock opens Label on its first row, whatever is set. */
            result = show_choice(LANG_EXTRAS_ALARMS, label_name,
                                 ARRAYLEN(alarm_label_names), 0, &a->label);
            break;
        case ROW_DELETE:
            delete_alarm(editing);
            return BACK;
    }
    return result == LEFT ? LEFT : STAY;
}

static int show_alarm(void)
{
    struct page page = {
        .title = LANG_EXTRAS_ALARMS, .count = ARRAYLEN(editor_rows),
        .name = editor_name, .kind = editor_kind, .value = editor_value,
        .choose = choose_editor_row, .pane = PANE_ALARM,
    };
    return show_page(&page);
}

static const char *sleep_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    if (!item)
        return str(LANG_OFF);
    snprintf(buf, size, str(LANG_EXTRAS_ALARMS_MINUTES), sleep_minutes[item]);
    return buf;
}

static int choose_sleep(struct page *page, int row)
{
    (void)page;
    set_sleeptimer_duration(sleep_minutes[row]);
    return BACK;
}

static int show_sleep_timer(void)
{
    int current = 0;
    for (unsigned i = 0; i < ARRAYLEN(sleep_minutes); i++)
        if (get_sleep_timer_active() &&
            sleep_minutes[i] * 60 == get_sleep_timer_duration())
            current = i;
    struct page page = {
        .title = LANG_EXTRAS_ALARMS, .count = ARRAYLEN(sleep_minutes),
        .selection = current, .name = sleep_name, .kind = kind_option,
        .choose = choose_sleep, .pane = PANE_SLEEP,
    };
    return show_page(&page);
}

static const char *main_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    const char *title, *sub;
    tall_texts(item, &title, &sub, buf, size);
    return title;
}

static int choose_main_row(struct page *page, int row)
{
    int result;
    if (row == 0)
    {
        if (!create_alarm())
            return STAY;
        result = show_alarm();
    }
    else if (row == 1)
        result = show_sleep_timer();
    else
    {
        editing = row - 2;
        result = show_alarm();
    }
    page->count = 2 + alarm_table.count;
    return result == LEFT ? LEFT : STAY;
}

int extras_alarms(void)
{
    alarm_table_load();
    push_current_activity(ACTIVITY_TIMEDATESCREEN);
    sub_font = ui_theme_current_font_load(UI_FONT_BODY,
                                          global_settings.glyphs_to_cache);
    caption_font = font_load_ex(CAPTION_FONT, 0,
                                global_settings.glyphs_to_cache);
    struct page page = {
        .title = LANG_EXTRAS_ALARMS, .count = 2 + alarm_table.count,
        .name = main_name, .kind = kind_menu, .choose = choose_main_row,
        .pane = PANE_MAIN, .tall = true,
    };
    int result = show_page(&page);
    font_unload(sub_font);
    font_unload(caption_font);
    sub_font = caption_font = -1;
    pop_current_activity();
    return result == LEFT ? GO_TO_ROOT : 0;
}
