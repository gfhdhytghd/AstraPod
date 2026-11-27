/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Calendars, as stock draws it: a menu of All Calendars, To Do's
 * and the event alarm's sound beside a pane of the time and today's
 * calendar page; a month of stock's cells, the wheel moving a day and
 * Previous and Next a month; and a day's page, Previous and Next moving a
 * day. The events are the calendar plugin's day memos, read from its file
 * and shown, not edited. To Do's has none, so it shows stock's instructions.
 *
 * Parts: the date arithmetic (the calendar plugin's), the memos, the
 * calendar page and pane, the pages composed a band at a time, then the
 * month, the day, To Do's and the menu.
 ****************************************************************************/
#include <stdio.h>
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
#include "widgets/list.h"
#include "root_menu.h"
#include "extras_calendars.h"
#include "extras.h"
#include "extras_font.h"

#define MEMO_FILE PLUGIN_APPS_DATA_DIR "/.memo"
#define MEMO_MAX 128
#define CAL_FONT FONT_DIR "/14-helvetica-bold-calendar.pbf"

#define WHITE 0xffffff
#define BLACK 0x000000
#define GREY 0x666666                /* weekday heads, other months' days */
#define BODY_GREY 0x474747           /* To Do's paragraphs */

/* The calendar page: its size, the month's baseline and the day's origin
 * and baseline below its top-left. */
#define ICON_W 130
#define MONTH_BASE 36
#define DAY_X 40
#define DAY_BASE 90
/* AstraPod_CalendarDigits_Image: ten cells, the strip's top this far above
 * the baseline. */
#define DIGIT_W 27
#define DIGIT_TOP 35
/* The pane: the page's top-left and the time's and weekday's baselines. */
#define PANE_ICON_X 176
#define PANE_ICON_Y 56
#define PANE_MID (MENU_PANE_X + (LCD_WIDTH - MENU_PANE_X) / 2)
#define TIME_BASE 34
#define WEEKDAY_BASE 221
/* The month: six rows of seven 41x30 cells under the weekday heads, each
 * number's origin and baseline inside its cell, and its event flag. */
#define CELL_X 4
#define CELL_Y 38
#define CELL_DX 45
#define CELL_DY 34
#define CELL_W 41
#define HEAD_BASE 34
#define NUM_X 4
#define NUM_BASE 15
#define FLAG_X 26
#define FLAG_Y 4
/* A day's page and To Do's. */
#define NO_EVENTS_BASE 114
#define EVENT_X 5
#define EVENT_TOP 20
#define EVENT_ROW 24
#define EVENT_MAX 8
#define TODO_X 24
#define TODO_W 280                   /* a line's width from TODO_X */
#define TODO_HEAD_BASE 71
#define TODO_BODY_BASE 93
#define TODO_PARA_GAP 31             /* a paragraph's last baseline to next */
#define BAND_ROWS 16

/* ---- dates ---- */

struct cal_date {
    int year, month, day;            /* month 1-12, day 1-31 */
};

static bool leap_year(int year)
{
    return (!(year % 4) && (year % 100)) || !(year % 400);
}

static int month_days(int year, int month)
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return days[month - 1] + (month == 2 && leap_year(year));
}

/* 0 for Sunday. */
static int week_day(const struct cal_date *d)
{
    static const unsigned char shift[12] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = d->year - (d->month < 3);
    return (y + y / 4 - y / 100 + y / 400 + shift[d->month - 1] + d->day) % 7;
}

static void step_days(struct cal_date *d, int days)
{
    d->day += days;
    while (d->day < 1)
    {
        if (--d->month < 1)
        {
            d->month = 12;
            d->year--;
        }
        d->day += month_days(d->year, d->month);
    }
    while (d->day > month_days(d->year, d->month))
    {
        d->day -= month_days(d->year, d->month);
        if (++d->month > 12)
        {
            d->month = 1;
            d->year++;
        }
    }
}

/* The same day of the next or previous month, or its last day. */
static void step_month(struct cal_date *d, int months)
{
    d->month += months;
    if (d->month < 1)
    {
        d->month = 12;
        d->year--;
    }
    else if (d->month > 12)
    {
        d->month = 1;
        d->year++;
    }
    d->day = MIN(d->day, month_days(d->year, d->month));
}

static struct cal_date today(void)
{
    const struct tm *tm = get_time();
    return (struct cal_date){ tm->tm_year + 1900, tm->tm_mon + 1,
                              tm->tm_mday };
}

/* `lang` with M, D and Y replaced by `month`, `day` and `year`. */
static void format_date(char *buf, size_t size, int lang, const char *month,
                        int day, int year)
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

/* ---- memos ----
 * The calendar plugin keeps one memo a line, "DDMMYYYYWT" then the text:
 * the day it was made, its weekday from Monday as 0, and T, which says it
 * comes back each week (0), month (1) or year (2), or once (3). */

struct memo {
    unsigned short year;
    unsigned char day, month, wday, type;
};

static struct memo memos[MEMO_MAX];
static int memo_count;

/* Read a memo's fields from `line`; false if it is not one. */
static bool memo_parse(const char *line, struct memo *m)
{
    for (int i = 0; i < 10; i++)
        if (line[i] < '0' || line[i] > '9')
            return false;
    m->day = (line[0] - '0') * 10 + line[1] - '0';
    m->month = (line[2] - '0') * 10 + line[3] - '0';
    m->year = ((line[4] - '0') * 10 + line[5] - '0') * 100 +
              (line[6] - '0') * 10 + line[7] - '0';
    m->wday = line[8] - '0';
    m->type = line[9] - '0';
    return m->day >= 1 && m->day <= 31 && m->month >= 1 && m->month <= 12 &&
           m->wday <= 6 && m->type <= 3;
}

static bool memo_on(const struct memo *m, const struct cal_date *d)
{
    switch (m->type)
    {
        case 0:
            return m->wday == (week_day(d) + 6) % 7;
        case 1:
            return m->day == d->day;
        case 2:
            return m->day == d->day && m->month == d->month;
        default:
            return m->day == d->day && m->month == d->month &&
                   m->year == d->year;
    }
}

/* Call `found` with the text of each memo on `d`, or with `d` NULL read
 * every memo into memos[]. */
static void memo_scan(const struct cal_date *d,
                      void (*found)(const char *text, void *data), void *data)
{
    char line[96];
    int fd = open(MEMO_FILE, O_RDONLY);
    if (!d)
        memo_count = 0;
    while (fd >= 0 && read_line(fd, line, sizeof(line)) > 0)
    {
        struct memo m;
        if (!memo_parse(line, &m))
            continue;
        if (!d && memo_count < MEMO_MAX)
            memos[memo_count++] = m;
        else if (d && memo_on(&m, d))
            found(line + 10, data);
    }
    if (fd >= 0)
        close(fd);
}

static bool has_memo(const struct cal_date *d)
{
    for (int i = 0; i < memo_count; i++)
        if (memo_on(&memos[i], d))
            return true;
    return false;
}

/* ---- the calendar page and the pane ---- */

/* The pane's title font, kept loaded as the pane keeps its own. */
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

/* `text` with its origin at x and its baseline at `base`. */
static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int base, uint32_t rgb)
{
    if (font >= 0)
        stock_canvas_text(c, font, text, x, base - font_get(font)->ascent,
                          rgb);
}

/* Black digit `n` of the strip, its coverage blended as stock blends text. */
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
    struct cal_date d = today();
    char time[16];
    if (global_settings.timeformat == 0)
        snprintf(time, sizeof(time), "%d:%02d", tm->tm_hour, tm->tm_min);
    else
        snprintf(time, sizeof(time), "%d:%02d %s", (tm->tm_hour + 11) % 12 + 1,
                 tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    int font = pane_font();
    put_centred(band, font, time, PANE_MID, TIME_BASE, WHITE);
    extras_calendars_icon_put(band, PANE_ICON_X, PANE_ICON_Y, d.month, d.day);
    put_centred(band, font, str(weekday[week_day(&d)]), PANE_MID,
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

/* ---- pages ----
 * A page is composed at 8 bits a channel, BAND_ROWS rows at a time, below
 * the status bar the theme draws. */

struct page {
    void (*compose)(struct stock_canvas *band, struct page *p);
    uint32_t *band;
    struct cal_date shown;           /* the month's selection, or the day */
    int font, head_font, body_font;
    int ret;                         /* a key's GO_TO_* that leaves */
    int events;
    char event[EVENT_MAX][64];       /* the day's memos */
};

static void page_paint(const struct ui_view *view,
                       const struct ui_view_frame *frame)
{
    struct page *p = view->data;
    struct ui_rect clip = frame->clip;
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            p->band, clip.x, y, clip.width,
            MIN(BAND_ROWS, clip.y + clip.height - y),
        };
        for (int i = 0; i < band.width * band.height; i++)
            band.pixels[i] = WHITE;
        p->compose(&band, p);
        stock_canvas_show(&band, clip);
    }
}

static const struct ui_view_ops page_ops = { .paint = page_paint };

/* Runs a page under `title` until Menu: `key` sees every other action and
 * returns whether the page changed, setting the new title. Returns
 * GO_TO_ROOT after USB, else 0. */
static int page_run(struct page *p, char *title, size_t size,
                    bool (*key)(struct page *p, int action, char *title,
                                size_t size))
{
    struct viewport content;
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int handle = core_alloc(LCD_WIDTH * BAND_ROWS * sizeof(uint32_t));
    int ret = 0;

    if (handle <= 0)
        return 0;
    p->band = core_get_data_pinned(handle);
    ui_scene_open_page(&scene, "calendars-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    struct ui_rect bounds = { content.x, content.y, content.width,
                              content.height };
    ui_view_init(&view, bounds, &page_ops, p);
    ui_page_init(&page, &view);

    for (bool dirty = true; ; )
    {
        if (dirty)
        {
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
        }
        int action = get_action(CONTEXT_SETTINGS, HZ);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
        dirty = key && key(p, action, title, size);
        if (p->ret)
        {
            ret = p->ret;
            break;
        }
        if (dirty)
        {
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            sb_update(SCREEN_MAIN, true);
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "calendars-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    core_put_data_pinned(p->band);
    core_free(handle);
    return ret;
}

/* ---- the month ---- */

static void month_title(const struct cal_date *d, char *buf, size_t size)
{
    format_date(buf, size, LANG_EXTRAS_CALENDARS_MONTH_TITLE,
                str(LANG_MONTH_JANUARY + d->month - 1), d->day, d->year);
}

static void month_compose(struct stock_canvas *band, struct page *p)
{
    struct cal_date first = { p->shown.year, p->shown.month, 1 };
    int lead = week_day(&first);
    struct cal_date d = first;
    step_days(&d, -(lead ? lead : 7));
    for (int col = 0; col < 7; col++)
    {
        const char *head = str(LANG_WEEKDAY_SUNDAY + col);
        int x = CELL_X + col * CELL_DX, font = extras_font_for(p->font, head);
        put_text(band, font, head,
                 x + (CELL_W - text_width(font, head)) / 2, HEAD_BASE, GREY);
    }
    for (int i = 0; i < 42; i++, step_days(&d, 1))
    {
        int x = CELL_X + i % 7 * CELL_DX, y = CELL_Y + i / 7 * CELL_DY;
        if (y >= band->y + band->height || y + CELL_DY <= band->y)
            continue;
        bool in = d.month == p->shown.month;
        bool selected = in && d.day == p->shown.day;
        stock_art_put(band, selected ? STOCK_ART_CALENDAR_HILITE_CELL :
                      in ? STOCK_ART_CALENDAR_ACTIVE_CELL :
                      STOCK_ART_CALENDAR_INACTIVE_CELL, x, y);
        char num[4];
        snprintf(num, sizeof(num), "%d", d.day);
        put_text(band, p->font, num, x + NUM_X, y + NUM_BASE,
                 selected ? WHITE : in ? BLACK : GREY);
        if (has_memo(&d))
            stock_art_put(band, STOCK_ART_CALENDAR_FLAG, x + FLAG_X,
                          y + FLAG_Y);
    }
}

static int day_page(struct cal_date *d, int font);

static bool month_key(struct page *p, int action, char *title, size_t size)
{
    switch (action)
    {
        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
            step_days(&p->shown, 1);
            break;
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
            step_days(&p->shown, -1);
            break;
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            step_month(&p->shown, 1);
            break;
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            step_month(&p->shown, -1);
            break;
        case ACTION_STD_OK:
            p->ret = day_page(&p->shown, p->body_font);
            break;
        default:
            return false;
    }
    month_title(&p->shown, title, size);
    return true;
}

static int month_page(void)
{
    char title[48];
    static struct page p;
    p = (struct page){ .compose = month_compose, .shown = today() };
    p.font = font_load_ex(CAL_FONT, 0, global_settings.glyphs_to_cache);
    if (p.font < 0)
        p.font = ui_theme_current_font_load(UI_FONT_LIST_VALUE,
                                            global_settings.glyphs_to_cache);
    p.body_font = pane_font();
    memo_scan(NULL, NULL, NULL);
    month_title(&p.shown, title, sizeof(title));
    int ret = page_run(&p, title, sizeof(title), month_key);
    font_unload(p.font);
    return ret;
}

/* ---- a day ---- */

static void day_event(const char *text, void *data)
{
    struct page *p = data;
    if (p->events < EVENT_MAX)
        strlcpy(p->event[p->events++], text, sizeof(p->event[0]));
}

/* Its memos a row each, as a list shows them, or stock's note. */
static void day_compose(struct stock_canvas *band, struct page *p)
{
    for (int i = 0; i < p->events; i++)
        put_text(band, p->head_font, p->event[i], EVENT_X,
                 EVENT_TOP + i * EVENT_ROW + 3 + font_get(p->head_font)->ascent,
                 BLACK);
    if (!p->events)
        put_centred(band, p->font, str(LANG_EXTRAS_CALENDARS_NO_EVENTS),
                    LCD_WIDTH / 2, NO_EVENTS_BASE, BLACK);
}

static void day_load(struct page *p, char *title, size_t size)
{
    char month[24];
    strlcpy(month, str(LANG_EXTRAS_CALENDARS_JANUARY + p->shown.month - 1),
            sizeof(month));
    format_date(title, size, LANG_EXTRAS_CALENDARS_DAY_TITLE, month,
                p->shown.day, p->shown.year);
    p->events = 0;
    memo_scan(&p->shown, day_event, p);
}

static bool day_key(struct page *p, int action, char *title, size_t size)
{
    if (action != ACTION_STD_NEXT && action != ACTION_STD_PREV)
        return false;
    step_days(&p->shown, action == ACTION_STD_NEXT ? 1 : -1);
    day_load(p, title, size);
    return true;
}

/* Previous and Next move the day, and `d` follows it. */
static int day_page(struct cal_date *d, int font)
{
    char title[48];
    static struct page p;
    p = (struct page){ .compose = day_compose, .shown = *d, .font = font };
    p.head_font = ui_theme_current_font_load(UI_FONT_LIST,
                                             global_settings.glyphs_to_cache);
    day_load(&p, title, sizeof(title));
    int ret = page_run(&p, title, sizeof(title), day_key);
    font_unload(p.head_font);
    *d = p.shown;
    return ret;
}

/* ---- To Do's ---- */

/* `text` wrapped from the baseline `base`; returns the last line's. */
static int put_paragraph(struct stock_canvas *c, int font, const char *text,
                         int base)
{
    struct font *pf = font_get(font);
    int pitch = pf->height;
    char line[160];
    font_lock(font, true);
    for (const char *at = text, *end; *at; base += pitch)
    {
        const char *next = text_wrap_next(at, TODO_W, pf, &end);
        strlcpy(line, at, MIN((size_t)(end - at) + 1, sizeof(line)));
        put_text(c, font, line, TODO_X, base, BODY_GREY);
        at = next;
        if (!*at)
            break;
    }
    font_lock(font, false);
    return base;
}

static void todo_compose(struct stock_canvas *band, struct page *p)
{
    put_text(band, p->head_font, str(LANG_EXTRAS_CALENDARS_TODO_HOWTO),
             TODO_X, TODO_HEAD_BASE, BLACK);
    int base = put_paragraph(band, p->body_font,
                             str(LANG_EXTRAS_CALENDARS_TODO_TEXT),
                             TODO_BODY_BASE);
    put_paragraph(band, p->body_font, str(LANG_EXTRAS_NOTES_HOWTO_MORE),
                  base + TODO_PARA_GAP);
}

static int todo_page(void)
{
    char title[48];
    static struct page p;
    p = (struct page){ .compose = todo_compose };
    p.head_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                             global_settings.glyphs_to_cache);
    p.body_font = ui_theme_current_font_load(UI_FONT_BODY,
                                             global_settings.glyphs_to_cache);
    strlcpy(title, str(LANG_EXTRAS_NOTES_INSTRUCTIONS), sizeof(title));
    int ret = page_run(&p, title, sizeof(title), NULL);
    font_unload(p.head_font);
    font_unload(p.body_font);
    return ret;
}

/* ---- the menu ---- */

enum { ROW_ALL, ROW_TODOS, ROW_ALARMS, ROW_COUNT };
/* The event alarm's sound, Beep, None or Off, kept until the player
 * restarts: no event sounds one. */
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
                ret = selected == ROW_ALL ? month_page() : todo_page();
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
    return ret;
}
