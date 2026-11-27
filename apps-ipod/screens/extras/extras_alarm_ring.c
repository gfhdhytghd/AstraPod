/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Rings the alarms that Extras > Alarms sets. get_action() asks
 * extras_alarm_ring_poll() before it waits; that reads the clock once a
 * minute, and an alarm whose minute has come shows its alert over the
 * screen: the label as the title, Dismiss and Snooze, and the clock with the
 * alarm's time and its label's sentence beside them. Playback pauses for the
 * alert and resumes after it. A Beep alarm beeps until it is answered.
 *
 * Only the UI thread rings, and never over a plugin or game that draws the
 * whole LCD; an alarm due meanwhile rings when the UI next waits outside one.
 * The player rings only while it is on: nothing wakes it for an alarm.
 *
 * Parts: the table and its file; the clock; what is due; the alert.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "backlight.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "audio/beep.h"
#include "input/action.h"
#include "draw/viewport.h"
#include "playlist/catalog.h"
#include "playlist/playlist.h"
#include "plugins/rock_compat.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "widgets/dialog.h"
#include "widgets/list.h"
#include "extras_alarm_ring.h"
#include "extras_bar.h"
#include "extras_calendars.h"

#define ALARMS_FILE ROCKBOX_DIR "/alarms.dat"
#define ALARMS_MAGIC_V1 0x314d4c41u /* "ALM1" */
#define ALARMS_MAGIC    0x324d4c41u /* "ALM2" */
#define SNOOZE_MINUTES 10
/* A clock that moves this many minutes more than the ticks say was set. */
#define CLOCK_SET 2
/* The most minutes examined at once, for a plugin that ran a day. */
#define LOOK_BACK 1440

/* The alert's pane: the clock, the time under it, then the sentence. */
#define ART_X 178
#define ART_Y 40
#define TEXT_X (MENU_PANE_X + (LCD_WIDTH - MENU_PANE_X) / 2)
#define TEXT_W 144
#define TIME_BASELINE 192
#define SENTENCE_BASELINE 210
#define SENTENCE_LEADING 15
#define TIME_FONT FONT_DIR "/17-helvetica-bold.pbf"
#define WHITE 0xffffff
/* Two short beeps a second. */
#define BEEP_FREQUENCY 2000
#define BEEP_MS 120
#define BEEP_AMPLITUDE 8000
#define BEAT (HZ / 5)

struct alarm_table alarm_table;
const int alarm_label_names[ALARM_LABELS] = {
    LANG_EXTRAS_ALARMS_WAKE_UP, LANG_EXTRAS_ALARMS_WORK,
    LANG_EXTRAS_ALARMS_CLASS, LANG_EXTRAS_ALARMS_ALARM,
    LANG_EXTRAS_ALARMS_APPOINTMENT, LANG_EXTRAS_ALARMS_PHONE_CALL,
    LANG_EXTRAS_ALARMS_BREAKFAST, LANG_EXTRAS_ALARMS_LUNCH,
    LANG_EXTRAS_ALARMS_DINNER, LANG_EXTRAS_ALARMS_IMPORTANT,
    LANG_EXTRAS_ALARMS_PRESCRIPTION, LANG_EXTRAS_ALARMS_TAKE_MEDICINE,
    LANG_EXTRAS_ALARMS_RETURN_CALL, LANG_EXTRAS_ALARMS_DELIVERY,
    LANG_EXTRAS_ALARMS_PICK_UP, LANG_EXTRAS_ALARMS_MEETING,
    LANG_EXTRAS_ALARMS_REMINDER, LANG_EXTRAS_ALARMS_DATE,
    LANG_EXTRAS_ALARMS_ANNIVERSARY, LANG_EXTRAS_ALARMS_BIRTHDAY,
    LANG_EXTRAS_ALARMS_HOLIDAY, LANG_EXTRAS_ALARMS_GO_HOME,
    LANG_EXTRAS_ALARMS_PARTY,
};
static const int label_sentences[ALARM_LABELS] = {
    LANG_EXTRAS_ALARMS_DESC_WAKE_UP, LANG_EXTRAS_ALARMS_DESC_WORK,
    LANG_EXTRAS_ALARMS_DESC_CLASS, LANG_EXTRAS_ALARMS_DESC_ALARM,
    LANG_EXTRAS_ALARMS_DESC_APPOINTMENT, LANG_EXTRAS_ALARMS_DESC_PHONE_CALL,
    LANG_EXTRAS_ALARMS_DESC_BREAKFAST, LANG_EXTRAS_ALARMS_DESC_LUNCH,
    LANG_EXTRAS_ALARMS_DESC_DINNER, LANG_EXTRAS_ALARMS_DESC_IMPORTANT,
    LANG_EXTRAS_ALARMS_DESC_PRESCRIPTION,
    LANG_EXTRAS_ALARMS_DESC_TAKE_MEDICINE,
    LANG_EXTRAS_ALARMS_DESC_RETURN_CALL, LANG_EXTRAS_ALARMS_DESC_DELIVERY,
    LANG_EXTRAS_ALARMS_DESC_PICK_UP, LANG_EXTRAS_ALARMS_DESC_MEETING,
    LANG_EXTRAS_ALARMS_DESC_REMINDER, LANG_EXTRAS_ALARMS_DESC_DATE,
    LANG_EXTRAS_ALARMS_DESC_ANNIVERSARY, LANG_EXTRAS_ALARMS_DESC_BIRTHDAY,
    LANG_EXTRAS_ALARMS_DESC_HOLIDAY, LANG_EXTRAS_ALARMS_DESC_GO_HOME,
    LANG_EXTRAS_ALARMS_DESC_PARTY,
};

static bool loaded, ringing;
static long pending;            /* the first minute not examined in full */
static unsigned pending_alarm;  /* the first alarm of it not examined */
static long last_minute, last_tick;     /* the previous clock reading */
static long next_look;          /* current_tick of the next examination */
static struct alarm snoozed;
static long snooze_at = -1;     /* the minute a snoozed alarm rings again */

/* ---- the clock, in seconds since 2000-01-01 ---- */

static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = y / 400;
    unsigned yoe = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 730425;
}

static void civil_from_days(long z, int *m, int *d)
{
    z += 730425;
    long era = z / 146097;
    unsigned doe = z - era * 146097;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
}

#ifdef SIMULATOR
/* PODBOX_ALARM_CLOCK="<offset>[,<speed>]", in the simulator only: the
 * alarms' clock starts <offset> seconds from the real one at its first
 * reading and runs <speed> times as fast. */
static long fake_base = -1, fake_tick, fake_speed = 1;
#endif

static long clock_now(void)
{
    struct tm *tm = get_time();
    long s = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1,
                             tm->tm_mday) * 86400L +
             tm->tm_hour * 3600L + tm->tm_min * 60 + tm->tm_sec;
#ifdef SIMULATOR
    const char *fake = getenv("PODBOX_ALARM_CLOCK");
    if (fake)
    {
        if (fake_base < 0)
        {
            char *end;
            fake_base = s + strtol(fake, &end, 10);
            fake_speed = *end == ',' ? MAX(1, strtol(end + 1, NULL, 10)) : 1;
            fake_tick = current_tick;
        }
        s = fake_base + (current_tick - fake_tick) * fake_speed / HZ;
    }
#endif
    return s;
}

/* Ticks from the clock reading `now` to the next minute. */
static int ticks_to_minute(long now)
{
    long ticks = (60 - now % 60) * HZ;
#ifdef SIMULATOR
    ticks /= fake_speed;
#endif
    return ticks + 1;
}

/* Clock minutes that `ticks` make. */
static long minutes_in(long ticks)
{
#ifdef SIMULATOR
    ticks *= fake_speed;
#endif
    return ticks / (60 * HZ);
}

/* ---- the table and its file ---- */

static void write_table(void)
{
    int fd = open(ALARMS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, &alarm_table, sizeof(alarm_table));
    close(fd);
}

/* Alarms ring from the next minute on, never for one already begun. */
static void look_from_now(void)
{
    loaded = true;
    last_minute = clock_now() / 60;
    last_tick = current_tick;
    pending = last_minute + 1;
    pending_alarm = 0;
    next_look = current_tick;
}

/* Legacy ALM1 disk format without playlist name, for seamless migration. */
struct alarm_v1 {
    uint8_t on, repeat, sound, label;
    uint8_t hour, minute, month, day;
    uint16_t year;
};

struct alarm_table_v1 {
    uint32_t magic;
    uint32_t count;
    struct alarm_v1 list[MAX_ALARMS];
};

void alarm_table_load(void)
{
    int fd = open(ALARMS_FILE, O_RDONLY);
    bool ok = false;
    if (fd >= 0)
    {
        uint32_t magic = 0;
        if (read(fd, &magic, sizeof(magic)) == (ssize_t)sizeof(magic))
        {
            lseek(fd, 0, SEEK_SET);
            if (magic == ALARMS_MAGIC)
            {
                ok = read(fd, &alarm_table, sizeof(alarm_table)) ==
                     (ssize_t)sizeof(alarm_table) &&
                     alarm_table.count <= MAX_ALARMS;
            }
            else if (magic == ALARMS_MAGIC_V1)
            {
                struct alarm_table_v1 v1;
                if (read(fd, &v1, sizeof(v1)) == (ssize_t)sizeof(v1) &&
                    v1.count <= MAX_ALARMS)
                {
                    memset(&alarm_table, 0, sizeof(alarm_table));
                    alarm_table.count = v1.count;
                    for (unsigned i = 0; i < v1.count; i++)
                    {
                        alarm_table.list[i].on = v1.list[i].on;
                        alarm_table.list[i].repeat = v1.list[i].repeat;
                        alarm_table.list[i].sound = v1.list[i].sound;
                        alarm_table.list[i].label = v1.list[i].label;
                        alarm_table.list[i].hour = v1.list[i].hour;
                        alarm_table.list[i].minute = v1.list[i].minute;
                        alarm_table.list[i].month = v1.list[i].month;
                        alarm_table.list[i].day = v1.list[i].day;
                        alarm_table.list[i].year = v1.list[i].year;
                    }
                    ok = true;
                }
            }
        }
        close(fd);
    }
    if (!ok)
        alarm_table.count = 0;
    alarm_table.magic = ALARMS_MAGIC;
    look_from_now();
}

void alarm_table_save(void)
{
    write_table();
    look_from_now();
}

/* ---- what is due ---- */

static bool rings_at(const struct alarm *a, long minute)
{
    long day = minute / 1440;
    if (minute % 1440 != a->hour * 60 + a->minute)
        return false;
    long first = days_from_civil(a->year, a->month, a->day);
    if (day < first)
        return false;
    int weekday = (day + 6) % 7;        /* 2000-01-01 was a Saturday */
    int month, date;
    civil_from_days(day, &month, &date);
    switch (a->repeat)
    {
        case ALARM_ONCE:        return day == first;
        case ALARM_EVERY_DAY:   return true;
        case ALARM_WEEKDAYS:    return weekday >= 1 && weekday <= 5;
        case ALARM_WEEKENDS:    return weekday == 0 || weekday == 6;
        case ALARM_EVERY_WEEK:  return (day - first) % 7 == 0;
        case ALARM_EVERY_MONTH: return date == a->day;
        case ALARM_EVERY_YEAR:  return month == a->month && date == a->day;
    }
    return false;
}

static bool armed(void)
{
    if (snooze_at >= 0 || extras_calendars_alarms_armed())
        return true;
    for (unsigned i = 0; i < alarm_table.count; i++)
        if (alarm_table.list[i].on)
            return true;
    return false;
}

/* The next alarm due by `minute` that has not rung, into `due`. A Once
 * alarm is switched off as it rings. */
static bool find_due(long minute, struct alarm *due)
{
    long moved = minute - last_minute;
    if (moved > minutes_in(current_tick - last_tick) + CLOCK_SET ||
        moved < -CLOCK_SET)
    {
        /* The clock was set: ring from its new minute, nothing it skipped. */
        pending = minute;
        pending_alarm = 0;
    }
    last_minute = minute;
    last_tick = current_tick;
    if (snooze_at >= 0 && minute >= snooze_at)
    {
        *due = snoozed;
        snooze_at = -1;
        return true;
    }
    if (pending < minute - LOOK_BACK)
    {
        pending = minute - LOOK_BACK;
        pending_alarm = 0;
    }
    for (; pending <= minute; pending++, pending_alarm = 0)
        for (unsigned i = pending_alarm; i < alarm_table.count; i++)
        {
            struct alarm *a = &alarm_table.list[i];
            if (!a->on || !rings_at(a, pending))
                continue;
            *due = *a;
            if (a->repeat == ALARM_ONCE)
            {
                a->on = 0;
                write_table();
            }
            pending_alarm = i + 1;
            return true;
        }
    return false;
}

/* A plugin or game that draws the whole LCD is in front. */
static bool lcd_taken(void)
{
    enum current_activity top = get_current_activity();
    return rock_ui_raw() || rock_ui_native() || top == ACTIVITY_PLUGIN ||
           top == ACTIVITY_SPIKE || top == ACTIVITY_MUSICQUIZ;
}

/* ---- the alert ---- */

static const struct alarm *alert;
static const char *alert_custom_title;
static const char *alert_custom_sentence;
static int time_font = -1, sentence_font = -1;

static void centred_text(struct stock_canvas *band, int font,
                         const char *text, int baseline)
{
    if (font < 0 || !*text)
        return;
    int width;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    stock_canvas_text(band, font, text, TEXT_X - width / 2,
                      baseline - font_get(font)->ascent, WHITE);
}

static void paint_alert_text(struct stock_canvas *band)
{
    char text[48];
    if (global_settings.timeformat == 0)
        snprintf(text, sizeof(text), "%02d:%02d", alert->hour, alert->minute);
    else
        snprintf(text, sizeof(text),
                 str(alert->hour < 12 ? LANG_EXTRAS_ALARMS_TIME_AM :
                                        LANG_EXTRAS_ALARMS_TIME_PM),
                 (alert->hour + 11) % 12 + 1, alert->minute);
    centred_text(band, time_font, text, TIME_BASELINE);
    if (sentence_font < 0)
        return;
    struct dialog_text_line lines[2];
    const char *sentence = alert_custom_sentence ? alert_custom_sentence :
                           (const char *)str(label_sentences[alert->label]);
    int count = dialog_wrap_text(sentence, TEXT_W, sentence_font, lines, 2);
    for (int i = 0; i < count; i++)
    {
        snprintf(text, sizeof(text), "%.*s", lines[i].len, lines[i].str);
        centred_text(band, sentence_font, text,
                     SENTENCE_BASELINE + i * SENTENCE_LEADING);
    }
}

static enum list_pane_kind alert_pane(int item, void *data, int *art)
{
    (void)item;
    (void)data;
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = STOCK_ART_ALARMS_CLOCK_STATIC,
        .x = ART_X, .y = ART_Y, .paint = paint_alert_text,
    };
    menu_pane_set_picture(&c);
    *art = c.art;
    return LIST_PANE_PICTURE;
}

static const char *alert_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    return str(item == 0 ? LANG_EXTRAS_ALARMS_DISMISS :
                           LANG_EXTRAS_ALARMS_SNOOZE);
}

static enum list_row_kind alert_kind(int item, void *data)
{
    (void)item;
    (void)data;
    return LIST_ROW_OPTION;
}

/* Show the alert for `a` until it is answered; true for Snooze. */
static bool show_alert(const struct alarm *a, bool allow_snooze,
                       const char *custom_title, const char *custom_sentence,
                       bool beep_active)
{
    char title[64];
    enum themable_icons icon;
    bool persistent = sb_get_title(SCREEN_MAIN, title, sizeof(title), &icon);
    const char *label = custom_title ? custom_title :
                        (const char *)str(alarm_label_names[a->label]);
    alert = a;
    alert_custom_title = custom_title;
    alert_custom_sentence = custom_sentence;
    time_font = font_load_ex(TIME_FONT, 0, global_settings.glyphs_to_cache);
    sentence_font = ui_theme_current_font_load(UI_FONT_BODY,
                                               global_settings.glyphs_to_cache);
    push_current_activity(ACTIVITY_TIMEDATESCREEN);
    sb_set_persistent_title(label, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);

    struct gui_synclist list;
    gui_synclist_init(&list, alert_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, (char *)label, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, alert_kind, NULL, NULL);
    gui_synclist_set_pane_callback(&list, alert_pane);
    gui_synclist_set_nb_items(&list, allow_snooze ? 2 : 1);
    gui_synclist_select_item(&list, 0);
    gui_synclist_draw_settled(&list);

    bool snooze = false;
    long start = current_tick, beat = -1;
    while (true)
    {
        long now = (current_tick - start) / BEAT;
        if (beep_active && now != beat)
        {
            beat = now;
            if (beat % 5 == 0 || beat % 5 == 2)
                beep_play(BEEP_FREQUENCY, BEEP_MS, BEEP_AMPLITUDE);
        }
        int action;
        extras_list_do_action(BEAT, &list, &action);
        if (action == ACTION_STD_OK)
        {
            snooze = allow_snooze && gui_synclist_get_sel_pos(&list) == 1;
            break;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (action == ACTION_REDRAW)
            gui_synclist_draw(&list);
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    beep_play(0, 0, 0);

    gui_synclist_scroll_stop(&list);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (persistent)
        sb_set_persistent_title(title, icon, SCREEN_MAIN);
    else
        sb_set_title_text(title, icon, SCREEN_MAIN);
    pop_current_activity();
    font_unload(time_font);
    time_font = sentence_font = -1;
    alert_custom_title = alert_custom_sentence = NULL;
    return snooze;
}

static bool start_alarm_playlist(const char *name)
{
    if (!name || !name[0])
        return false;
    char dir[MAX_PATH], leaf[ALARM_PLAYLIST_LEN + 8], full[MAX_PATH];
    catalog_get_directory(dir, sizeof(dir));
    if (!dir[0])
        return false;
    static const char *const exts[2] = { ".m3u8", ".m3u" };
    for (int i = 0; i < 2; i++)
    {
        snprintf(leaf, sizeof(leaf), "%s%s", name, exts[i]);
        snprintf(full, sizeof(full), "%s/%s", dir, leaf);
        if (file_exists(full))
        {
            if (playlist_create(dir, leaf) >= 0 && playlist_amount() > 0)
            {
                playlist_start(0, 0, 0);
                return true;
            }
        }
    }
    return false;
}

static void ring(const struct alarm *a)
{
    ringing = true;
    backlight_on();
    bool started_playlist = false;
    bool resume = false;
    if (a->sound == ALARM_SOUND_PLAYLIST)
        started_playlist = start_alarm_playlist(a->playlist);
    if (!started_playlist)
    {
        resume = (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
                 == AUDIO_STATUS_PLAY;
        if (resume)
            audio_pause();
    }
    bool beep_active = (a->sound == ALARM_SOUND_BEEP) ||
                       (a->sound == ALARM_SOUND_PLAYLIST && !started_playlist);
    if (show_alert(a, true, NULL, NULL, beep_active))
    {
        snoozed = *a;
        snooze_at = clock_now() / 60 + SNOOZE_MINUTES;
    }
    if (resume)
        audio_resume();
    ringing = false;
}

static void ring_calendar(const char *summary, int hour, int minute,
                          int sound_mode)
{
    struct alarm fake;
    memset(&fake, 0, sizeof(fake));
    fake.hour = (uint8_t)hour;
    fake.minute = (uint8_t)minute;
    ringing = true;
    backlight_on();
    bool resume = (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
                  == AUDIO_STATUS_PLAY;
    if (resume)
        audio_pause();
    show_alert(&fake, false, str(LANG_EXTRAS_CALENDARS), summary,
               sound_mode == 0);
    if (resume)
        audio_resume();
    ringing = false;
}

bool extras_alarm_ring_poll(int *timeout)
{
    if (ringing || !talk_on_ui_thread() || lcd_taken())
        return false;
    if (!loaded)
        alarm_table_load();
    if (!armed())
        return false;
    if (TIME_AFTER(next_look, current_tick))
    {
        long wait = next_look - current_tick;
        if (*timeout == TIMEOUT_BLOCK || *timeout > wait)
            *timeout = wait;
        return false;
    }
    long now = clock_now();
    next_look = current_tick + ticks_to_minute(now);
    struct alarm due;
    if (find_due(now / 60, &due))
    {
        ring(&due);
        next_look = current_tick;   /* another may be due in the same minute */
        return true;
    }
    char cal_summary[64];
    int cal_h = 0, cal_m = 0, cal_sound = 0;
    if (extras_calendars_due_event(now / 60, cal_summary, sizeof(cal_summary),
                                   &cal_h, &cal_m, &cal_sound))
    {
        ring_calendar(cal_summary, cal_h, cal_m, cal_sound);
        next_look = current_tick;
        return true;
    }
    if (*timeout == TIMEOUT_BLOCK || *timeout > next_look - current_tick)
        *timeout = next_look - current_tick;
    return false;
}
