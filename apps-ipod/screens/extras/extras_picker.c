/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reusable Date and Time pickers for Extras screens.
 ****************************************************************************/
#include "config.h"
#include "button.h"
#include "system.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "fixedpoint.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "input/action.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "ui/native_theme_runtime.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "extras_bar.h"
#include "extras_calendars.h"
#include "extras_font.h"
#include "extras_picker.h"

#define DIGIT_FONT      FONT_DIR "/21-helvetica-bold.pbf"
#define CAPTION_FONT    FONT_DIR "/17-helvetica-bold.pbf"
#define CLOCK_X         14
#define CLOCK_Y         52
#define CLOCK_CX        80
#define CLOCK_CY        118
#define CALENDAR_X      16
#define CALENDAR_Y      56
#define BAR_X           150
#define BAR_Y           97
#define WELL_Y          101
#define WELL_W          40
#define SELECTION_Y     103
#define DIGIT_BASELINE  129
#define CAPTION_X       80
#define CAPTION_BASE    205
#define CAPTION_LEAD    21
#define BAND_ROWS       EXTRAS_BAND_ROWS
#define WHITE           0xffffff

static const int well_x[3] = { 165, 213, 261 };

struct picker_state {
    bool is_date;
    int field;
    int value[3];
    int min_year, max_year;
    const char *title;
    const char *caption;
    int digit_font, caption_font;
};

static bool is_24h(void)
{
    return global_settings.timeformat == 0;
}

static bool is_stock_look(void)
{
    return ui_theme_style(UI_STYLE_ART) == UI_ART_PLASTIC && stock_art_ready();
}

void extras_format_time(char *buf, size_t size, int hour, int minute)
{
    if (is_24h())
        snprintf(buf, size, "%d:%02d", hour, minute);
    else
        snprintf(buf, size, str(hour < 12 ? LANG_EXTRAS_ALARMS_TIME_AM :
                                            LANG_EXTRAS_ALARMS_TIME_PM),
                 (hour + 11) % 12 + 1, minute);
}

static void centred_text(struct stock_canvas *band, int font,
                         const char *text, int cx, int baseline, uint32_t rgb)
{
    if (font < 0 || !text || !*text)
        return;
    int width = 0;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    stock_canvas_text(band, font, text, cx - width / 2,
                      baseline - font_get(font)->ascent, rgb);
}

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

static void slot_text(const struct picker_state *s, int slot,
                      char *buf, size_t size)
{
    int v = s->value[slot];
    if (s->is_date)
    {
        if (slot == 2)
            snprintf(buf, size, "%02d", v % 100);
        else
            snprintf(buf, size, "%d", v);
    }
    else if (slot == 0)
        snprintf(buf, size, "%d", is_24h() ? v : (v + 11) % 12 + 1);
    else if (slot == 1)
        snprintf(buf, size, "%02d", v);
    else
        strlcpy(buf, str(v ? LANG_EXTRAS_ALARMS_PM : LANG_EXTRAS_ALARMS_AM),
                size);
}

static int picker_hour24(const struct picker_state *s)
{
    if (is_24h())
        return s->value[0];
    return s->value[0] % 12 + (s->value[2] ? 12 : 0);
}

static void compose_stock(struct stock_canvas *band,
                          const struct picker_state *s)
{
    stock_art_put(band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    extras_bar_put(band, s->title);
    if (s->is_date)
        extras_calendars_icon_put(band, CALENDAR_X, CALENDAR_Y,
                                  s->value[0], s->value[1]);
    else
    {
        int h = picker_hour24(s), m = s->value[1];
        stock_art_put(band, STOCK_ART_CLOCK_LARGE, CLOCK_X, CLOCK_Y);
        put_hand(band, h % 12 * 30 + m / 2, 18, 40, 16);
        put_hand(band, m * 6, 29, 32, 8);
        put_cap(band);
    }

    int slots = s->is_date || !is_24h() ? 3 : 2;
    stock_art_put(band, STOCK_ART_DATETIMEPICKER_BAR, BAR_X, BAR_Y);
    for (int i = 0; i < slots; i++)
        stock_art_put(band, STOCK_ART_DATETIMEPICKER_WELL, well_x[i], WELL_Y);
    stock_art_put(band, STOCK_ART_DATETIMEPICKER_SELECTION,
                  well_x[s->field] + 1, SELECTION_Y);

    for (int i = 0; i < slots; i++)
    {
        char text[16];
        slot_text(s, i, text, sizeof(text));
        centred_text(band, extras_font_for(s->digit_font, text), text,
                     well_x[i] + WELL_W / 2, DIGIT_BASELINE, WHITE);
    }

    char summary[32];
    if (s->is_date)
        snprintf(summary, sizeof(summary), "%s %d, %04d",
                 str(LANG_MONTH_JANUARY + s->value[0] - 1),
                 s->value[1], s->value[2]);
    else
        extras_format_time(summary, sizeof(summary), picker_hour24(s),
                           s->value[1]);
    centred_text(band, s->caption_font, summary, CAPTION_X, CAPTION_BASE,
                 WHITE);
    if (s->caption && *s->caption)
        centred_text(band, s->caption_font, s->caption, CAPTION_X,
                     CAPTION_BASE + CAPTION_LEAD, WHITE);
}

static void paint_scrim(const struct picker_state *s)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport content;
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    unsigned bg = ui_theme_rgb565(UI_COLOR_BACKGROUND);
    unsigned fg = ui_theme_rgb565(UI_COLOR_FOREGROUND);
    unsigned muted = ui_theme_rgb565(UI_COLOR_MUTED);
    unsigned well_bg = LCD_RGBPACK(26, 29, 36);
    unsigned sel_bg = LCD_RGBPACK(36, 88, 168);
    unsigned sel_fg = LCD_WHITE;
    unsigned sep = ui_theme_rgb565(UI_COLOR_SEPARATOR);

    content.fg_pattern = fg;
    content.bg_pattern = bg;
    content.drawmode = DRMODE_SOLID;
    struct viewport *last = d->set_viewport(&content);
    d->clear_viewport();

    int slots = s->is_date || !is_24h() ? 3 : 2;
    int bw = 64, bh = 44, gap = 14;
    int total_w = slots * bw + (slots - 1) * gap;
    int sx = (content.width - total_w) / 2;
    int sy = (content.height - bh) / 2 - 14;

    static const char *const date_lbl[3] = { "Month", "Day", "Year" };
    static const char *const time_lbl[3] = { "Hour", "Min", "AM/PM" };

    for (int i = 0; i < slots; i++)
    {
        int x = sx + i * (bw + gap);
        bool active = (i == s->field);
        content.fg_pattern = active ? sel_bg : well_bg;
        d->fillrect(x, sy, bw, bh);
        content.fg_pattern = active ? sel_fg : sep;
        d->drawrect(x, sy, bw, bh);

        char val[16];
        if (s->is_date && i == 2)
            snprintf(val, sizeof(val), "%04d", s->value[2]);
        else
            slot_text(s, i, val, sizeof(val));

        int font = s->digit_font >= 0 ? s->digit_font : content.font;
        content.font = font;
        content.fg_pattern = active ? sel_fg : fg;
        content.drawmode = DRMODE_FG;
        int w = 0, h = 0;
        font_getstringsize((const unsigned char *)val, &w, &h, font);
        d->putsxy(x + (bw - w) / 2, sy + (bh - h) / 2,
                  (const unsigned char *)val);

        const char *lbl = s->is_date ? date_lbl[i] : time_lbl[i];
        int lfont = s->caption_font >= 0 ? s->caption_font : content.font;
        content.font = lfont;
        content.fg_pattern = muted;
        font_getstringsize((const unsigned char *)lbl, &w, &h, lfont);
        d->putsxy(x + (bw - w) / 2, sy - h - 6, (const unsigned char *)lbl);
        content.drawmode = DRMODE_SOLID;
    }

    char summary[48];
    if (s->is_date)
        snprintf(summary, sizeof(summary), "%s %d, %04d",
                 str(LANG_MONTH_JANUARY + s->value[0] - 1),
                 s->value[1], s->value[2]);
    else
        extras_format_time(summary, sizeof(summary), picker_hour24(s),
                           s->value[1]);

    int cfont = s->caption_font >= 0 ? s->caption_font : content.font;
    content.font = cfont;
    content.fg_pattern = fg;
    content.drawmode = DRMODE_FG;
    int sw = 0, sh = 0;
    font_getstringsize((const unsigned char *)summary, &sw, &sh, cfont);
    d->putsxy((content.width - sw) / 2, sy + bh + 18,
              (const unsigned char *)summary);

    d->set_viewport(last);
    d->update();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
}

static void paint_picker(const struct picker_state *s)
{
    if (!is_stock_look())
    {
        paint_scrim(s);
        return;
    }
    uint32_t *pixels = extras_band(0);
    if (!pixels)
        return;
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS)
    {
        struct stock_canvas band = { pixels, 0, y, LCD_WIDTH, BAND_ROWS };
        compose_stock(&band, s);
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   BAND_ROWS });
    }
    lcd_update();
}

static void step_picker(struct picker_state *s, int delta)
{
    int *v = &s->value[s->field];
    if (s->is_date)
    {
        if (s->field == 0)
            *v = (*v + 11 + delta) % 12 + 1;
        else if (s->field == 1)
        {
            int days = extras_month_days(s->value[2], s->value[0]);
            *v = (*v + days - 1 + delta) % days + 1;
        }
        else
            *v = MIN(MAX(*v + delta, s->min_year), s->max_year);
        int maxd = extras_month_days(s->value[2], s->value[0]);
        if (s->value[1] > maxd)
            s->value[1] = maxd;
    }
    else if (s->field == 0)
    {
        int mod = is_24h() ? 24 : 12;
        *v = (*v + mod + delta) % mod;
    }
    else if (s->field == 1)
        *v = (*v + 60 + delta) % 60;
    else
        *v = !*v;
}

static int run_picker(struct picker_state *s)
{
    int fields = s->is_date || !is_24h() ? 3 : 2;
    int result = 0;
    bool stock = is_stock_look();
    struct ui_scene scene = {0};

    ui_scene_open_page(&scene, "extras-picker-push");
    if (stock)
        viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    else
    {
        sb_set_persistent_title(s->title, Icon_NOICON, SCREEN_MAIN);
        viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    }

    s->digit_font = font_load_ex(DIGIT_FONT, 0,
                                 global_settings.glyphs_to_cache);
    if (s->digit_font < 0)
        s->digit_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                                   global_settings.glyphs_to_cache);
    s->caption_font = font_load_ex(CAPTION_FONT, 0,
                                   global_settings.glyphs_to_cache);
    if (s->caption_font < 0)
        s->caption_font = ui_theme_current_font_load(UI_FONT_BODY,
                                                     global_settings.glyphs_to_cache);

    paint_picker(s);
    for (;;)
    {
        int action = get_action(CONTEXT_STD, HZ);
        int button;
        get_action_statuscode(&button);
        if (action == ACTION_STD_CANCEL && button == BUTTON_LEFT)
        {
            if (s->field > 0)
                --s->field;
            paint_picker(s);
        }
        else if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
        {
            step_picker(s, 1);
            paint_picker(s);
        }
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
        {
            step_picker(s, -1);
            paint_picker(s);
        }
        else if (action == ACTION_STD_OK)
        {
            if (++s->field >= fields)
            {
                result = 1;
                break;
            }
            paint_picker(s);
        }
        else if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            result = 0;
            break;
        }
        else if (action == ACTION_REDRAW)
            paint_picker(s);
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = -1;
            break;
        }
    }

    ui_scene_close_modal(&scene, result >= 0, "extras-picker-pop");
    if (s->digit_font >= 0)
        font_unload(s->digit_font);
    if (s->caption_font >= 0)
        font_unload(s->caption_font);
    if (stock)
        extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return result;
}

int extras_pick_date(const char *title, const char *caption,
                     struct extras_date *date, int min_year, int max_year)
{
    struct picker_state s = {
        .is_date = true,
        .min_year = min_year,
        .max_year = max_year,
        .title = title,
        .caption = caption,
        .digit_font = -1,
        .caption_font = -1,
    };
    s.value[0] = MIN(MAX(date->month, 1), 12);
    s.value[2] = MIN(MAX(date->year, min_year), max_year);
    s.value[1] = MIN(MAX(date->day, 1), extras_month_days(s.value[2], s.value[0]));

    int r = run_picker(&s);
    if (r == 1)
    {
        date->month = s.value[0];
        date->day = s.value[1];
        date->year = s.value[2];
    }
    return r;
}

int extras_pick_time(const char *title, const char *caption,
                     int *hour, int *minute)
{
    struct picker_state s = {
        .is_date = false,
        .title = title,
        .caption = caption,
        .digit_font = -1,
        .caption_font = -1,
    };
    int h = MIN(MAX(*hour, 0), 23);
    int m = MIN(MAX(*minute, 0), 59);
    s.value[0] = is_24h() ? h : h % 12;
    s.value[1] = m;
    s.value[2] = h >= 12 ? 1 : 0;

    int r = run_picker(&s);
    if (r == 1)
    {
        *hour = picker_hour24(&s);
        *minute = s.value[1];
    }
    return r;
}
