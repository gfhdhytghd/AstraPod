/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock Extras > Stopwatch. Play/Pause or Select starts a session on
 * the dial; while it runs Select records a lap and Play/Pause stops it and
 * opens the Stopwatch menu, beside a pane with the dial. The menu resumes
 * the session, starts a new one, clears the logs or opens one: a session's
 * total, its shortest, longest and average lap and every lap. Sessions live
 * until the player shuts down; a running one keeps time while away.
 *
 * Parts: the sessions; the dial, drawn from the stock bitmaps; the timer
 * page; a log; the menu; the entry.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "kernel.h"
#include "font.h"
#include "lcd.h"
#include "lang.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "input/action.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "ui/frame.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "root_menu.h"
#include "extras_bar.h"
#include "extras.h"
#include "ui/scene.h"

#define SW_LOGS 10
#define SW_LAPS 64
#define CS(ticks) ((long)(ticks) * 100 / HZ)

#define WHITE 0xffffff
#define BLACK 0x000000

/* Where stock draws the timer page; the pane is the same art moved right by
 * MENU_PANE_X. */
#define CLOCK_X 14
#define CLOCK_Y 35
#define SECS_X 24
#define SECS_Y 77
#define SECS_EDGE 124
#define MINS_X 77
#define MINS_Y 115
#define MINS_EDGE 18
#define BUTTON_X 174
#define BUTTON_Y 82
#define TIME_X 165
#define TIME_BASELINE 126
#define LAP_X 173
#define LAP_BASELINE 157
#define LAP_PITCH 24
#define LAPS_SHOWN 3
#define PANE_ICON_X 177
#define PANE_ICON_Y 212
#define PANE_TIME_X 207
#define PANE_TIME_BASELINE 228
/* The part of the page that changes as the time does. */
#define HANDS_RECT { SECS_X, SECS_Y, SECS_EDGE, SECS_EDGE }
#define TEXT_RECT { MENU_PANE_X, 95, LCD_WIDTH - MENU_PANE_X, \
                    LCD_HEIGHT - 95 }
#define BAND_ROWS EXTRAS_BAND_ROWS

/* A log page. */
#define LOG_HEADER_Y 20
#define LOG_STATS_Y 48
#define LOG_LAPS_Y 157
#define LOG_PITCH 24
#define LOG_HEADER_RGB LCD_RGBPACK(206, 223, 247)
#define LOG_STATS_RGB LCD_RGBPACK(247, 247, 247)
#define LOG_LABEL_RGB LCD_RGBPACK(148, 150, 148)
#define LOG_LAP_RGB LCD_RGBPACK(49, 134, 214)

/* ---- the sessions ---- */

/* A session's time is `total` plus, while it runs, the ticks since
 * `since`. A lap ends at lap_end[], in centiseconds of the session. */
struct sw_log {
    struct tm start;
    long total, since;
    bool running;
    int laps;
    long lap_end[SW_LAPS];
};

/* Newest first; logs[0] is the session on the dial while `current`. */
static struct sw_log logs[SW_LOGS];
static int log_count;
static bool current;

static long sw_elapsed(const struct sw_log *log)
{
    return CS(log->total + (log->running ? current_tick - log->since : 0));
}

static void sw_stop(struct sw_log *log)
{
    if (log->running)
        log->total += current_tick - log->since;
    log->running = false;
}

static void sw_run(struct sw_log *log)
{
    if (!log->running)
        log->since = current_tick;
    log->running = true;
}

static void sw_start(void)
{
    if (log_count == SW_LOGS)
        log_count--;
    memmove(&logs[1], &logs[0], log_count * sizeof(logs[0]));
    log_count++;
    memset(&logs[0], 0, sizeof(logs[0]));
    logs[0].start = *get_time();
    current = true;
    sw_run(&logs[0]);
}

static void sw_lap(struct sw_log *log)
{
    if (log->laps < SW_LAPS)
        log->lap_end[log->laps++] = sw_elapsed(log);
}

/* Lap `i` of a session, the one still running last; a session never lapped
 * has none. */
static int sw_lap_count(const struct sw_log *log)
{
    return log->laps ? log->laps + 1 : 0;
}

static long sw_lap_time(const struct sw_log *log, int i)
{
    long end = i < log->laps ? log->lap_end[i] : sw_elapsed(log);
    return end - (i ? log->lap_end[i - 1] : 0);
}

static bool sw_running(void)
{
    return current && logs[0].running;
}

/* "00:00:12" and ".98", or with `millis` "00:00:12.980". */
static void sw_format(long cs, char *whole, size_t size, char *part,
                      size_t part_size, bool millis)
{
    long s = cs / 100;
    snprintf(whole, size, "%02ld:%02ld:%02ld", s / 3600 % 100, s / 60 % 60,
             s % 60);
    if (millis)
        snprintf(part, part_size, ".%03ld", cs % 100 * 10);
    else
        snprintf(part, part_size, ".%02ld", cs % 100);
}

static void sw_clock_text(const struct tm *tm, char *buf, size_t size)
{
    if (global_settings.timeformat == 0)
        snprintf(buf, size, "%d:%02d", tm->tm_hour, tm->tm_min);
    else
        snprintf(buf, size, "%d:%02d %s", (tm->tm_hour + 11) % 12 + 1,
                 tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

/* ---- the dial ---- */

/* Stock sets its text in these sizes of Helvetica Bold. */
enum sw_font { TIMER_WHOLE, TIMER_PART, PANE_PART, LAP_WHOLE, LAP_PART,
               LOG_TEXT, LOG_LAP, SW_FONTS };
static const char *const font_file[SW_FONTS][2] = {
    [TIMER_WHOLE] = { "29-helvetica-bold", "29-helvetica-bold" },
    [TIMER_PART] = { "21-helvetica-bold-stopwatch", "21-helvetica-bold" },
    [PANE_PART] = { "14-helvetica-bold-calendar", "14-helvetica-bold" },
    [LAP_WHOLE] = { "23-helvetica-bold", "21-helvetica-bold" },
    [LAP_PART] = { "18-helvetica-bold", "17-helvetica-bold-pane" },
    [LOG_TEXT] = { "17-helvetica-bold-pane", "17-helvetica-bold-pane" },
    [LOG_LAP] = { "16-helvetica-bold", "16-helvetica-bold" },
};
static int fonts[SW_FONTS] = { -1, -1, -1, -1, -1, -1, -1 };

/* Each font, or its nearest size while the build lacks it. */
static void sw_fonts_load(void)
{
    char path[MAX_PATH];
    for (int i = 0; i < SW_FONTS; i++)
        for (int j = 0; j < 2 && fonts[i] < 0; j++)
        {
            snprintf(path, sizeof(path), FONT_DIR "/%s.pbf", font_file[i][j]);
            fonts[i] = font_load_ex(path, 0,
                                    global_settings.glyphs_to_cache);
        }
}

static void sw_fonts_release(void)
{
    for (int i = 0; i < SW_FONTS; i++)
    {
        font_unload(fonts[i]);
        fonts[i] = -1;
    }
}

/* Where frame f of a hand sits among its ids, which run in name order:
 * 0, 1, 10 to 19, 2, 20 to 29, 3 to 9. */
static const uint8_t frame_order[30] = {
    0, 1, 12, 23, 24, 25, 26, 27, 28, 29, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
};

/* Frame `k` of a hand's 120 steps round the dial. The bitmaps hold one
 * quarter turn; the others are the same frames turned by whole quarters
 * about the bitmap's centre. */
static void put_hand(struct stock_canvas *c, enum stock_art_id first,
                     int edge, int k, int x, int y)
{
    k %= 120;
    const struct stock_art *m = &stock_art[first + frame_order[k % 30]];
    int quarter = k / 30, n = edge - 1;
    int y0 = MAX(y, c->y), y1 = MIN(y + edge, c->y + c->height);
    for (int v = y0 - y; v < y1 - y; v++)
        for (int u = 0; u < edge; u++)
        {
            int row = quarter == 0 ? v : quarter == 1 ? n - u :
                      quarter == 2 ? n - v : u;
            int col = quarter == 0 ? u : quarter == 1 ? v :
                      quarter == 2 ? n - u : n - v;
            stock_canvas_blend(c, x + u, y + v, BLACK,
                               stock_art_alpha(m, col, row));
        }
}

static void put_time(struct stock_canvas *c, long cs, int x, int baseline,
                     int whole_font, int part_font, uint32_t rgb)
{
    char whole[16], part[8];
    sw_format(cs, whole, sizeof(whole), part, sizeof(part), false);
    stock_canvas_text(c, whole_font, whole, x,
                      baseline - font_get(whole_font)->ascent, rgb);
    int width;
    font_getstringsize((const unsigned char *)whole, &width, NULL, whole_font);
    stock_canvas_text(c, part_font, part, x + width,
                      baseline - font_get(part_font)->ascent, rgb);
}

/* The dial with its hands at `cs`, at x + `left`. */
static void put_dial(struct stock_canvas *c, int left, long cs)
{
    if (!stock_art_ready())
        return;

    stock_art_put(c, STOCK_ART_STOPWATCH_CLOCK, CLOCK_X + left, CLOCK_Y);
    put_hand(c, STOCK_ART_STOPWATCH_SECS_IMAGE_0, SECS_EDGE, cs / 50,
             SECS_X + left, SECS_Y);
    put_hand(c, STOCK_ART_STOPWATCH_MINS_IMAGE_0, MINS_EDGE, cs / 3000,
             MINS_X + left, MINS_Y);
}

void extras_stopwatch_pane_paint(struct stock_canvas *band)
{
    put_dial(band, MENU_PANE_X, current ? sw_elapsed(&logs[0]) : 0);
}

/* Lap rows fade with age: stock draws each in this colour at this alpha. */
#define LAP_RGB 0xdce6fa
static const uint8_t lap_alpha[LAPS_SHOWN] = { 120, 54, 30 };

/* The `i`th newest lap, composed on a copy of the band and laid over it at
 * its row's alpha. */
static void put_lap(struct stock_canvas *c, const struct sw_log *log, int i)
{
    uint32_t *faded = extras_band(1);
    int baseline = LAP_BASELINE + i * LAP_PITCH;
    if (c->y >= baseline + 6 || c->y + c->height <= baseline - 22)
        return;
    int n = c->width * c->height;
    struct stock_canvas f = *c;
    f.pixels = faded;
    memcpy(faded, c->pixels, n * sizeof(faded[0]));
    put_time(&f, sw_lap_time(log, log->laps - 1 - i), LAP_X, baseline,
             fonts[LAP_WHOLE], fonts[LAP_PART], LAP_RGB);
    for (int p = 0; p < n; p++)
        if (faded[p] != c->pixels[p])
            stock_canvas_blend(c, c->x + p % c->width, c->y + p / c->width,
                               faded[p] & 0xffffff, lap_alpha[i]);
}

static void compose_page(struct stock_canvas *c)
{
    const struct sw_log *log = &logs[0];
    long cs = current ? sw_elapsed(log) : 0;
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    put_dial(c, 0, cs);
    if (!current)
        stock_art_put(c, STOCK_ART_STOPWATCH_PLAYPAUSE, BUTTON_X, BUTTON_Y);
    else
    {
        put_time(c, cs, TIME_X, TIME_BASELINE, fonts[TIMER_WHOLE],
                 fonts[TIMER_PART], WHITE);
        for (int i = 0; i < LAPS_SHOWN && i < log->laps; i++)
            put_lap(c, log, i);
    }
    extras_bar_put(c, str(LANG_EXTRAS_STOPWATCH));
}

static void compose_pane(struct stock_canvas *c)
{
    long cs = current ? sw_elapsed(&logs[0]) : 0;
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    put_dial(c, MENU_PANE_X, cs);
    if (current)
    {
        stock_art_put(c, logs[0].running ? STOCK_ART_STOPWATCH_SMALLPLAY :
                      STOCK_ART_STOPWATCH_SMALLPAUSE, PANE_ICON_X,
                      PANE_ICON_Y);
        put_time(c, cs, PANE_TIME_X, PANE_TIME_BASELINE, fonts[TIMER_PART],
                 fonts[PANE_PART], WHITE);
    }
    stock_art_put(c, STOCK_ART_PREVIEWAREA_SHADOW, MENU_PANE_X, 0);
}

/* Compose `clip` a band at a time and write it to the framebuffer. */
static void paint(struct ui_rect clip, void (*compose)(struct stock_canvas *))
{
    uint32_t *band = extras_band(0);
    if (!band)
        return;
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas c = { band, clip.x, y, clip.width,
                                  MIN(BAND_ROWS, clip.y + clip.height - y) };
        compose(&c);
        stock_canvas_show(&c, clip);
    }
    lcd_update_rect(clip.x, clip.y, clip.width, clip.height);
}

/* ---- the timer page ---- */

enum sw_next { SW_MENU, SW_EXIT, SW_USB };

static enum sw_next sw_timer(void)
{
    const struct ui_rect all = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    const struct ui_rect hands = HANDS_RECT, text = TEXT_RECT;
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    sw_fonts_load();
    paint(all, compose_page);
    long shown = current ? sw_elapsed(&logs[0]) : 0;
    enum sw_next next;
    while (1)
    {
        int action = get_action(CONTEXT_STD, sw_running() ? HZ / 25 :
                                TIMEOUT_BLOCK);
        if (action == ACTION_TREE_WPS || action == ACTION_STD_OK)
        {
            if (!current)
                sw_start();
            else if (action == ACTION_STD_OK)
                sw_lap(&logs[0]);
            else
            {
                sw_stop(&logs[0]);
                next = SW_MENU;
                break;
            }
            paint(all, compose_page);
        }
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
        {
            next = log_count ? SW_MENU : SW_EXIT;
            break;
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            next = SW_USB;
            break;
        }
        long cs = current ? sw_elapsed(&logs[0]) : 0;
        if (cs / 50 != shown / 50 || cs / 3000 != shown / 3000)
            paint(hands, compose_page);
        if (cs != shown)
            paint(text, compose_page);
        shown = cs;
    }
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    extras_bar_release();
    sw_fonts_release();
    return next;
}

/* ---- a log ---- */

/* `text` with its baseline on `baseline`, starting at x or, if `right`,
 * ending there. */
static void log_text(struct viewport *vp, int font, const char *text, int x,
                     int baseline, bool right, unsigned rgb)
{
    if (right)
        x -= font_getstringsize((const unsigned char *)text, NULL, NULL,
                                font);
    vp->font = font;
    vp->fg_pattern = rgb;
    lcd_putsxy(x, baseline - font_get(font)->ascent,
               (const unsigned char *)text);
}

static void log_paint(const struct sw_log *log, int scroll)
{
    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    viewport_set_fullscreen(&vp, SCREEN_MAIN);
    vp.drawmode = DRMODE_FG;
    struct viewport *last = lcd_set_viewport(&vp);
    int top = LOG_HEADER_Y, text = fonts[LOG_TEXT];
    char a[32], b[16];

    vp.fg_pattern = LOG_HEADER_RGB;
    lcd_fillrect(0, top, LCD_WIDTH, LOG_STATS_Y - top);
    vp.fg_pattern = LCD_RGBPACK(198, 199, 198);
    lcd_hline(0, LCD_WIDTH - 1, LOG_STATS_Y);
    vp.fg_pattern = LOG_STATS_RGB;
    lcd_fillrect(0, LOG_STATS_Y + 1, LCD_WIDTH, LOG_LAPS_Y - LOG_STATS_Y - 2);
    vp.fg_pattern = LCD_RGBPACK(231, 231, 231);
    lcd_hline(0, LCD_WIDTH - 1, LOG_LAPS_Y - 1);

    static const int month[12] = {
        LANG_MONTH_JANUARY, LANG_MONTH_FEBRUARY, LANG_MONTH_MARCH,
        LANG_MONTH_APRIL, LANG_MONTH_MAY, LANG_MONTH_JUNE, LANG_MONTH_JULY,
        LANG_MONTH_AUGUST, LANG_MONTH_SEPTEMBER, LANG_MONTH_OCTOBER,
        LANG_MONTH_NOVEMBER, LANG_MONTH_DECEMBER,
    };
    snprintf(a, sizeof(a), "%s %d %d", str(month[log->start.tm_mon % 12]),
             log->start.tm_mday, log->start.tm_year + 1900);
    log_text(&vp, text, a, 8, 40, false, LCD_BLACK);
    sw_clock_text(&log->start, b, sizeof(b));
    log_text(&vp, text, b, 312, 40, true, LCD_BLACK);

    int laps = sw_lap_count(log);
    long shortest = 0, longest = 0;
    for (int i = 0; i < laps; i++)
    {
        long t = sw_lap_time(log, i);
        if (!i || t < shortest)
            shortest = t;
        longest = MAX(longest, t);
    }
    long stats[4] = { sw_elapsed(log), shortest, longest,
                      laps ? sw_elapsed(log) / laps : 0 };
    static const int label[4] = {
        LANG_EXTRAS_STOPWATCH_TOTAL, LANG_EXTRAS_STOPWATCH_SHORTEST,
        LANG_EXTRAS_STOPWATCH_LONGEST, LANG_EXTRAS_STOPWATCH_AVERAGE,
    };
    for (int i = 0; i < 4; i++)
    {
        int y = 72 + i * LOG_PITCH;
        log_text(&vp, text, str(label[i]), 8, y, false, LOG_LABEL_RGB);
        sw_format(stats[i], a, sizeof(a), b, sizeof(b), true);
        strlcat(a, b, sizeof(a));
        log_text(&vp, text, a, 312, y, true, LCD_BLACK);
    }

    struct viewport rows = vp;
    rows.y = LOG_LAPS_Y;
    rows.height = LCD_HEIGHT - LOG_LAPS_Y;
    lcd_set_viewport(&rows);
    rows.fg_pattern = LCD_WHITE;
    lcd_fillrect(0, 0, LCD_WIDTH, rows.height);
    for (int i = 0; i < laps; i++)
    {
        int y = i * LOG_PITCH + 16 - scroll;
        if (y + LOG_PITCH < 0 || y >= rows.height + LOG_PITCH)
            continue;
        snprintf(a, sizeof(a), str(LANG_EXTRAS_STOPWATCH_LAP), i + 1);
        log_text(&rows, fonts[LOG_LAP], a, 5, y, false, LCD_BLACK);
        sw_format(sw_lap_time(log, i), a, sizeof(a), b, sizeof(b), false);
        log_text(&rows, fonts[PANE_PART], a, 314, y, true, LOG_LAP_RGB);
    }
    lcd_set_viewport(last);
    lcd_update_rect(0, LOG_HEADER_Y, LCD_WIDTH, LCD_HEIGHT - LOG_HEADER_Y);
}

static enum sw_next sw_log_page(const struct sw_log *log)
{
    struct ui_scene scene = {0};
    int scroll = 0;
    int laps = sw_lap_count(log);
    int most = MAX(0, laps * LOG_PITCH - (LCD_HEIGHT - LOG_LAPS_Y));
    ui_scene_open_page(&scene, "stopwatch-log-push");
    sb_set_persistent_title(str(LANG_EXTRAS_STOPWATCH_LOG), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    sw_fonts_load();
    enum sw_next next = SW_MENU;
    while (1)
    {
        log_paint(log, scroll);
        int action = get_action(CONTEXT_STD, log->running ? HZ / 25 :
                                HZ / 2);
        if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            scroll = MIN(most, scroll + LOG_PITCH);
        else if (action == ACTION_STD_PREV ||
                 action == ACTION_STD_PREVREPEAT)
            scroll = MAX(0, scroll - LOG_PITCH);
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            next = SW_USB;
            break;
        }
    }
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    sw_fonts_release();
    ui_scene_close_modal(&scene, next != SW_USB, "stopwatch-log-pop");
    return next;
}

/* ---- the menu ---- */

enum sw_row { ROW_RESUME, ROW_NEW, ROW_CLEAR, ROW_CURRENT, ROW_LOG };

/* What row `i` of the menu is; a log row's log is i - first_log(). */
static int first_log(void)
{
    return 1 + (current ? 1 : 0) + (log_count ? 1 : 0) + (current ? 1 : 0);
}

static enum sw_row row_of(int i)
{
    if (!current)
        i++;
    if (i >= 3 && !current)
        i++;
    if (i >= 2 && !log_count)
        i++;
    return i >= ROW_LOG ? ROW_LOG : (enum sw_row)i;
}

static const char *row_name(int i, void *data, char *buf, size_t size)
{
    (void)data;
    static const int name[ROW_LOG] = {
        LANG_BOOKMARK_CONTEXT_RESUME, LANG_EXTRAS_STOPWATCH_NEW_TIMER,
        LANG_EXTRAS_STOPWATCH_CLEAR_LOGS, LANG_EXTRAS_STOPWATCH_CURRENT_LOG,
    };
    enum sw_row row = row_of(i);
    if (row != ROW_LOG)
        return str(name[row]);
    const struct tm *tm = &logs[i - first_log()].start;
    snprintf(buf, size, "%d/%d/%02d", tm->tm_mon + 1, tm->tm_mday,
             tm->tm_year % 100);
    return buf;
}

static const char *row_value(int i, void *data, char *buf, size_t size)
{
    (void)data;
    if (row_of(i) != ROW_LOG)
        return NULL;
    sw_clock_text(&logs[i - first_log()].start, buf, size);
    return buf;
}

static enum list_row_kind row_kind(int i, void *data)
{
    (void)data;
    return row_of(i) == ROW_LOG ? LIST_ROW_SETTING : LIST_ROW_MENU;
}

static enum list_pane_kind row_pane(int i, void *data, int *art)
{
    (void)i; (void)data; (void)art;
    struct menu_pane_content pane = {
        .kind = LIST_PANE_PICTURE, .art = STOCK_ART_STOPWATCH_CLOCK,
        .x = CLOCK_X + MENU_PANE_X, .y = CLOCK_Y,
    };
    menu_pane_set_picture(&pane);
    return LIST_PANE_PICTURE;
}

/* The list draws the pane's backdrop and dial; the hands and time are
 * drawn over it. */
static void paint_pane(bool all)
{
    if (!menu_pane_split())
        return;
    const struct ui_rect pane = { MENU_PANE_X, 0, LCD_WIDTH - MENU_PANE_X,
                                  LCD_HEIGHT };
    const struct ui_rect hands = { SECS_X + MENU_PANE_X, SECS_Y, SECS_EDGE,
                                   SECS_EDGE };
    const struct ui_rect text = { MENU_PANE_X, PANE_ICON_Y - 2,
                                  LCD_WIDTH - MENU_PANE_X,
                                  LCD_HEIGHT - PANE_ICON_Y + 2 };
    if (all)
        paint(pane, compose_pane);
    else
    {
        paint(hands, compose_pane);
        paint(text, compose_pane);
    }
}

/* The row the menu last had selected, kept while the player is on: a log
 * row by the log it names. */
static enum sw_row menu_row;
static int menu_log;

static int menu_index(int count)
{
    for (int i = 0; i < count; i++)
        if (row_of(i) == menu_row &&
            (menu_row != ROW_LOG || i - first_log() == menu_log))
            return i;
    return current ? 1 : 0;
}

static enum sw_next sw_menu(void)
{
    struct gui_synclist list;
    int count = first_log() + log_count;
    sb_set_persistent_title(str(LANG_EXTRAS_STOPWATCH), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_init(&list, row_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, str(LANG_EXTRAS_STOPWATCH), Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, row_kind, row_value, NULL);
    gui_synclist_set_pane_callback(&list, row_pane);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_select_item(&list, menu_index(count));
    gui_synclist_draw_settled(&list);
    /* The list's first draw leaves the bar without its title. */
    sb_settle(SCREEN_MAIN);
    sw_fonts_load();
    paint_pane(true);
    unsigned flushes = ui_flush_count();
    long shown = current ? sw_elapsed(&logs[0]) : 0;
    long settled = current_tick + HZ;
    enum sw_next next = SW_EXIT;
    int action;
    while (1)
    {
        list_do_action(CONTEXT_TREE, sw_running() ? HZ / 25 : HZ / 10,
                       &list, &action);
        int i = gui_synclist_get_sel_pos(&list);
        menu_row = row_of(i);
        menu_log = i - first_log();
        if (action == ACTION_STD_OK)
        {
            next = SW_MENU;
            break;
        }
        if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            next = SW_USB;
            break;
        }
        /* Whatever the list paints covers the hands and the time, and it
         * settles its first page over a few frames. */
        long cs = current ? sw_elapsed(&logs[0]) : 0;
        if (action != ACTION_NONE || ui_flush_count() != flushes ||
            TIME_BEFORE(current_tick, settled))
            paint_pane(true);
        else if (cs != shown)
            paint_pane(false);
        flushes = ui_flush_count();
        shown = cs;
    }
    gui_synclist_scroll_stop(&list);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    sw_fonts_release();
    return next;
}

/* ---- the entry ---- */

int extras_stopwatch(void)
{
    static const char timer_page, menu_page;
    struct ui_scene entry = {0}, route = {0};
    bool timer_first = sw_running() || (!current && !log_count);
    ui_scene_open_page(&entry, "stopwatch-push");
    enum sw_next next = timer_first ? sw_timer() : SW_MENU;

    if (timer_first && next == SW_MENU)
        ui_scene_enter(&route, &timer_page, &menu_page, false, true);
    while (next == SW_MENU)
    {
        next = sw_menu();
        if (next != SW_MENU)
            break;
        switch (menu_row)
        {
            case ROW_RESUME:
                sw_run(&logs[0]);
                ui_scene_enter(&route, &menu_page, &timer_page, true, true);
                next = sw_timer();
                if (next == SW_MENU)
                    ui_scene_enter(&route, &timer_page, &menu_page, false, true);
                break;
            case ROW_NEW:
                if (current)
                    sw_stop(&logs[0]);
                current = false;
                ui_scene_enter(&route, &menu_page, &timer_page, true, true);
                next = sw_timer();
                if (next == SW_MENU)
                    ui_scene_enter(&route, &timer_page, &menu_page, false, true);
                break;
            case ROW_CLEAR:
                log_count = 0;
                current = false;
                break;
            case ROW_CURRENT:
                next = sw_log_page(&logs[0]);
                break;
            case ROW_LOG:
                next = sw_log_page(&logs[menu_log]);
                break;
        }
    }
    ui_scene_discard(&route);
    ui_scene_close_modal(&entry, next != SW_USB, "stopwatch-pop");
    return next == SW_USB ? MENU_ATTACHED_USB : 0;
}
