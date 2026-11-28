/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Fruit Company's Voice Memos, as the stock iPod Classic 2.0.5 has them.
 * The main menu shows the row, between Extras and Settings, while a headset
 * with a microphone is in the jack or a memo is on the disk. Its menu
 * starts a recording and sets its Quality while the microphone is in, then
 * lists Delete All and the memos, oldest first. The recording screen shows
 * the memo's number, date and length; Menu or Play/Pause pauses it into a
 * menu of Resume, Stop and Save and Delete, and a held Menu saves it and
 * goes to the main menu. Select on a memo offers Play and Delete, and
 * Play/Pause plays it. Memos are WAV files in /Recordings named by the time
 * they started, as stock names them.
 *
 * Parts: the memos on disk, the pane, the recorder, the recording screen,
 * the paused menu, a memo's menu, Delete All's question, the menu.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "audio.h"
#include "button.h"
#include "dir.h"
#include "file.h"
#include "font.h"
#include "lcd.h"
#include "lang.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "events.h"
#include "audio/pcm_record.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "playlist/playlist.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "ui/statusbar.h"
#include "ui/menu_pane.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/splash.h"
#include "root_menu.h"
#include "screens/recording.h"
#include "screens/extras/extras_bar.h"
#include "screens/fruit_menu.h"
#include "voice_memos.h"

#define MEMO_FONT       FONT_DIR "/25-helvetica-bold.pbf"
#define DATE_FONT       FONT_DIR "/17-helvetica-bold.pbf"
#define MIC_X           24
#define MIC_Y           27
#define TEXT_X          150
#define MEMO_BASELINE   90
#define DATE_BASELINE   108
#define DIGIT_Y         140
#define COLON_Y         145
#define PANE_MIC_X      187
#define PANE_MIC_Y      27
#define PANE_BASELINE   213
#define BAND_ROWS       EXTRAS_BAND_ROWS

/** The memos on disk **/

/* The memos the menu lists, at most; more are counted but not listed */
#define LISTED          250
#define NAME_LEN        15      /* "YYYYMMDD HHMMSS" */
#define FIXED_ROWS      3       /* Start Recording, Quality, Delete All */

/* A memo's name: "YYYYMMDD HHMMSS.wav" */
static bool memo_name(const char *name)
{
    if (strlen(name) != 19 || name[8] != ' ' || strcasecmp(name + 15, ".wav"))
        return false;
    for (int i = 0; i < 15; i++)
        if (i != 8 && (name[i] < '0' || name[i] > '9'))
            return false;
    return true;
}

static volatile int memos = -1;     /* how many, or -1 until counted */
static int listed;
static char names[LISTED][NAME_LEN + 1];

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* Count again once the disk has been a computer's */
static void forget(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    memos = -1;
}

static int memo_count(void)
{
    static bool subscribed;
    if (!subscribed)
        subscribed = add_event(SYS_EVENT_USB_EXTRACTED, forget);
    if (memos >= 0)
        return memos;
    int count = 0;
    listed = 0;
    DIR *dir = opendir(REC_BASE_DIR);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            if (!memo_name((const char *)entry->d_name))
                continue;
            if (listed < LISTED)
            {
                memcpy(names[listed], entry->d_name, NAME_LEN);
                names[listed++][NAME_LEN] = '\0';
            }
            count++;
        }
        closedir(dir);
    }
    qsort(names, listed, sizeof(names[0]), by_name);
    memos = count;
    return count;
}

static void memo_path(int i, char *buf, size_t size)
{
    snprintf(buf, size, "%s/%s.wav", REC_BASE_DIR, names[i]);
}

/* Stop playback that reads `path`, or any memo for NULL, before it goes */
static void release_playback(const char *path)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;
    if (!id3 || !id3->path[0])
        return;
    if (path ? !strcmp(id3->path, path) :
        !strncmp(id3->path, REC_BASE_DIR "/", sizeof(REC_BASE_DIR)))
        audio_stop();
}

static void delete_one(int i)
{
    char path[MAX_PATH];
    memo_path(i, path, sizeof(path));
    release_playback(path);
    remove(path);
    memos = -1;
}

static void delete_all(void)
{
    char path[MAX_PATH];
    release_playback(NULL);
    memo_count();
    /* names[] holds LISTED at most: go round until none is left, or none
     * of them goes */
    bool removed = true;
    while (listed > 0 && removed)
    {
        removed = false;
        for (int i = 0; i < listed; i++)
        {
            memo_path(i, path, sizeof(path));
            removed |= remove(path) == 0;
        }
        memos = -1;
        memo_count();
    }
}

/* "11/28 1:07 PM", from the memo's name */
static void memo_label(int i, char *buf, size_t size)
{
    const char *n = names[i];
    int month = (n[4] - '0') * 10 + n[5] - '0';
    int day = (n[6] - '0') * 10 + n[7] - '0';
    int hour = (n[9] - '0') * 10 + n[10] - '0';
    int minute = (n[11] - '0') * 10 + n[12] - '0';
    if (global_settings.timeformat == 0)
        snprintf(buf, size, "%d/%d %d:%02d", month, day, hour, minute);
    else
        snprintf(buf, size, "%d/%d %d:%02d %s", month, day,
                 (hour + 11) % 12 + 1, minute, hour < 12 ? "AM" : "PM");
}

/** The pane **/

enum state { IDLE, RECORDING, PAUSED };
static enum state state = IDLE;
static unsigned long paused_ticks;  /* the paused memo's length */

static bool mic(void)
{
    return mikey_mic_present();
}

static bool shown(void)
{
    return mic() || memo_count() > 0;
}

/* The rows the menus show, as one number */
static int rows_signature(void)
{
    return mic() | MIN(memo_count(), LISTED + 1) << 1;
}

bool voice_memos_refresh(const struct menu_item_ex *menu)
{
    static int last = -1;
    if (menu != fruit_menu_root() && menu != &voice_memos_menu)
        return false;
    int now = rows_signature();
    bool changed = last >= 0 && last != now;
    last = now;
    return changed;
}

bool voice_memos_paused(void)
{
    return state == PAUSED;
}

/* "M:SS", or "H:MM:SS" from an hour */
static void format_length(char *buf, size_t size, unsigned long seconds)
{
    if (seconds >= 3600)
        snprintf(buf, size, "%lu:%02lu:%02lu", seconds / 3600,
                 seconds / 60 % 60, seconds % 60);
    else
        snprintf(buf, size, "%lu:%02lu", seconds / 60, seconds % 60);
}

/** The menus, declared for the pane and the recorder **/

static const struct menu_item_ex paused_menu;
static const struct menu_item_ex memo_menu;

bool voice_memos_owns(const struct menu_item_ex *menu)
{
    return menu == &voice_memos_menu || menu == &paused_menu ||
           menu == &memo_menu;
}

/* The microphone, and under it the number of memos or the paused memo's
 * length */
bool voice_memos_pane(const struct menu_item_ex *menu,
                      const struct menu_item_ex *item)
{
    if (item != &voice_memos_menu && !voice_memos_owns(menu))
        return false;
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = STOCK_ART_VOICEMEMOSIMAGE_MICROPHONE,
        .x = PANE_MIC_X, .y = PANE_MIC_Y, .baseline = PANE_BASELINE,
    };
    if (menu == &paused_menu)
    {
        format_length(c.title, sizeof(c.title), paused_ticks / HZ);
        /* stock centres the length on the microphone */
        c.centre = PANE_MIC_X + 55;
        c.stamp = paused_ticks / HZ;
    }
    else
    {
        int count = memo_count();
        snprintf(c.title, sizeof(c.title),
                 str(count == 1 ? LANG_FRUIT_ONE_RECORDING :
                     LANG_FRUIT_RECORDING_COUNT), count);
        c.stamp = count;
    }
    menu_pane_set_picture(&c);
    return true;
}

/** The recorder **/

struct memo {
    char path[MAX_PATH];
    char title[32];
    char date[48];
    bool capture;       /* false where nothing records: the simulator */
    int keyclick;
    int memo_font, date_font;
};

/* The next memo's file, number and date. Its name is the time it starts. */
static void memo_name_now(struct memo *m)
{
    struct tm *tm = get_time();
    snprintf(m->path, sizeof(m->path), "%s/%04d%02d%02d %02d%02d%02d.wav",
             REC_BASE_DIR, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    snprintf(m->title, sizeof(m->title), str(LANG_FRUIT_MEMO),
             memo_count() + 1);
    snprintf(m->date, sizeof(m->date), "%s %d, %d",
             str(LANG_EXTRAS_CALENDARS_JANUARY + tm->tm_mon), tm->tm_mday,
             tm->tm_year + 1900);
}

static bool recorder_start(struct memo *m)
{
    m->capture = recording_capture_available();
    if (!m->capture)
        return true;
    if (!dir_exists(REC_BASE_DIR) && mkdir(REC_BASE_DIR) < 0)
        return false;
    if (!audio_try_init_recording())
    {
        if (audio_status() & AUDIO_STATUS_ERROR)
            audio_error_clear();
        return false;
    }
    struct audio_recording_options options = {
        .rec_source = AUDIO_SRC_MIC,
        .rec_frequency = global_settings.voice_memo_quality ?
                         REC_FREQ_44 : REC_FREQ_22,
        .rec_channels = 1,
        .rec_mono_mode = 1, /* left ADC */
        .enc_config.rec_format = REC_FORMAT_PCM_WAV,
    };
    rec_set_recording_options(&options);
    audio_set_recording_gain(global_settings.rec_mic_gain, 0, AUDIO_GAIN_MIC);
    audio_record(m->path);
    if ((audio_status() & (AUDIO_STATUS_RECORD | AUDIO_STATUS_ERROR)) ==
        AUDIO_STATUS_RECORD)
        return true;
    audio_close_recording();
    rec_set_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    sound_settings_apply();
    return false;
}

static unsigned long recorder_ticks(const struct memo *m)
{
    return m->capture ? audio_recorded_time() : 0;
}

static void recorder_pause(const struct memo *m, bool pause)
{
    if (!m->capture)
        return;
    if (pause)
        audio_pause_recording();
    else
        audio_resume_recording();
}

/* Close the memo, and keep it or delete it */
static void recorder_finish(const struct memo *m, bool keep)
{
    if (!m->capture)
        return;
    audio_stop_recording_sync();
    audio_close_recording();
    rec_set_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    sound_settings_apply();
    if (!keep)
        remove(m->path);
    memos = -1;
}

/** The recording screen **/

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int baseline)
{
    if (font >= 0)
        stock_canvas_text(c, font, text, x, baseline - font_get(font)->ascent,
                          0x000000);
}

/* HH:MM:SS in the stock digits, each in its own slot */
static void put_length(struct stock_canvas *c, unsigned long seconds)
{
    static const int slot[6] = { 146, 165, 195, 215, 245, 265 };
    static const int colon[2] = { 187, 237 };
    unsigned value[3] = { seconds / 3600 % 100, seconds / 60 % 60,
                          seconds % 60 };
    for (int i = 0; i < 6; i++)
    {
        unsigned v = value[i / 2];
        unsigned digit = i % 2 ? v % 10 : v / 10;
        stock_art_put(c, STOCK_ART_VOICEMEMOSIMAGE_REFLECT_0 + digit,
                      slot[i], DIGIT_Y);
    }
    for (int i = 0; i < 2; i++)
        stock_art_put(c, STOCK_ART_VOICEMEMOSIMAGE_COLON, colon[i], COLON_Y);
}

static void compose(struct stock_canvas *c, const struct memo *m,
                    unsigned long seconds)
{
    stock_art_put(c, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
    stock_art_put(c, STOCK_ART_VOICEMEMOSIMAGE_MICROPHONE, MIC_X, MIC_Y);
    put_text(c, m->memo_font, m->title, TEXT_X, MEMO_BASELINE);
    put_text(c, m->date_font, m->date, TEXT_X, DATE_BASELINE);
    put_length(c, seconds);
    extras_bar_put_ex(c, str(LANG_FRUIT_VOICE_MEMOS), true, false);
    stock_art_put(c, STOCK_ART_VOICEMEMOS_RECORD_STATUS, 271, 3);
}

/* Rows y0 to y1 of the screen */
static void paint(const struct memo *m, unsigned long seconds, int y0, int y1)
{
    uint32_t *band = extras_band(0);
    if (!band)
        return;
    const struct ui_rect all = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    for (int y = y0 / BAND_ROWS * BAND_ROWS; y < y1; y += BAND_ROWS)
    {
        struct stock_canvas c = { band, 0, y, LCD_WIDTH, BAND_ROWS };
        compose(&c, m, seconds);
        stock_canvas_show(&c, all);
    }
    lcd_update_rect(0, y0, LCD_WIDTH, y1 - y0);
}

/* Menu or Play/Pause pauses; a held Menu saves and leaves. */
static const struct button_mapping recording_map[] = {
    { ACTION_STD_CANCEL,     BUTTON_MENU|BUTTON_REL,    BUTTON_MENU },
    { ACTION_STD_MENU,       BUTTON_MENU|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_WPS_PLAY,       BUTTON_PLAY|BUTTON_REL,    BUTTON_PLAY },
    { ACTION_NONE,           BUTTON_PLAY|BUTTON_REPEAT, BUTTON_NONE },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping *get_recording_map(int context)
{
    (void)context;
    return recording_map;
}

/** The paused menu **/

enum outcome { RESUME, SAVE, SAVE_AND_LEAVE, DELETE };
static enum outcome outcome;

static int choose(enum outcome chosen)
{
    outcome = chosen;
    return 1;
}

static int resume(void)         { return choose(RESUME); }
static int stop_and_save(void)  { return choose(SAVE); }
static int delete_memo(void)    { return choose(DELETE); }

#define ROW(name, lang, func) \
    MENUITEM_FUNCTION(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), func, NULL, \
                      Icon_NOICON)

ROW(resume_row, LANG_FRUIT_RESUME, resume);
ROW(save_row, LANG_FRUIT_STOP_AND_SAVE, stop_and_save);
ROW(delete_row, LANG_DELETE, delete_memo);

/* Menu and Play/Pause resume, and a held Menu saves and leaves, as on the
 * recording screen */
static int paused_callback(int action, const struct menu_item_ex *this_item,
                           struct gui_synclist *this_list)
{
    (void)this_list;
    if (this_item != &paused_menu)
        return action;
    if (action == ACTION_STD_MENU || action == ACTION_TREE_WPS)
    {
        outcome = RESUME;
        return ACTION_STD_CANCEL;
    }
    if (action == ACTION_STD_QUICKSCREEN)
    {
        outcome = SAVE_AND_LEAVE;
        return ACTION_STD_CANCEL;
    }
    return action;
}

static const struct menu_item_ex *const paused_rows[] = {
    &resume_row, &save_row, &delete_row,
};
static const struct menu_callback_with_desc paused_desc = {
    paused_callback, ID2P(LANG_FRUIT_VOICE_MEMOS), Icon_NOICON };
static const struct menu_item_ex paused_menu = {
    MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(ARRAYLEN(paused_rows)),
    { (void *)paused_rows }, { .callback_and_desc = &paused_desc } };

/* The paused menu over the recording; what was chosen, or USB */
static int paused(const struct memo *m)
{
    recorder_pause(m, true);
    paused_ticks = recorder_ticks(m);
    state = PAUSED;
    outcome = RESUME;
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    int selected = 0;
    int ret = do_menu(&paused_menu, &selected, NULL, false);
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    state = RECORDING;
    if (ret == MENU_ATTACHED_USB)
        return MENU_ATTACHED_USB;
    if (outcome == RESUME)
        recorder_pause(m, false);
    return outcome;
}

/* Record a memo until it is saved or deleted: 0 back to the Voice Memos
 * menu, or a way out of the menus */
static int record(void)
{
    struct memo m;
    memo_name_now(&m);
    m.keyclick = global_settings.keyclick;
    global_settings.keyclick = 0;
    talk_disable(true);
    if (!recorder_start(&m))
    {
        global_settings.keyclick = m.keyclick;
        talk_disable(false);
        splash(HZ, ID2P(LANG_REC_CAPTURE_ERROR));
        return 0;
    }
    m.memo_font = font_load_ex(MEMO_FONT, 0, 16);
    m.date_font = font_load_ex(DATE_FONT, 0, 32);

    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "voice-memo-push");
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    state = RECORDING;
    unsigned long shown = 0;
    paint(&m, shown, 0, LCD_HEIGHT);

    int ret = 0;
    bool keep = true;
    while (true)
    {
        unsigned long seconds = recorder_ticks(&m) / HZ;
        if (seconds != shown)
        {
            shown = seconds;
            paint(&m, shown, DIGIT_Y, DIGIT_Y + 50);
        }
        if (!mic() || (m.capture && (audio_status() & AUDIO_STATUS_ERROR)))
            break;
        int action = get_custom_action(CONTEXT_PLUGIN | CONTEXT_CUSTOM,
                                       HZ / 5, get_recording_map);
        if (action == ACTION_STD_MENU)
        {
            ret = GO_TO_ROOT;
            break;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_WPS_PLAY)
        {
            int chosen = paused(&m);
            if (chosen == MENU_ATTACHED_USB)
            {
                ret = MENU_ATTACHED_USB;
                break;
            }
            if (chosen == SAVE_AND_LEAVE)
                ret = GO_TO_ROOT;
            if (chosen != RESUME)
            {
                keep = chosen != DELETE;
                break;
            }
            paint(&m, shown, 0, LCD_HEIGHT);
        }
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = MENU_ATTACHED_USB;
            break;
        }
    }

    state = IDLE;
    recorder_finish(&m, keep);
    global_settings.keyclick = m.keyclick;
    talk_disable(false);
    ui_scene_close_modal(&scene, ret != MENU_ATTACHED_USB, "voice-memo-pop");
    if (m.memo_font >= 0)
        font_unload(m.memo_font);
    if (m.date_font >= 0)
        font_unload(m.date_font);
    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/** A memo's menu **/

static int chosen;          /* the memo row last entered */
static bool play_chosen;    /* Play/Pause entered it: play, not ask */
static bool deleted;        /* its menu deleted it */

/* Play the memo on its own, in Now Playing */
static int play_row_func(void)
{
    char path[MAX_PATH];
    memo_path(chosen, path, sizeof(path));
    if (playlist_create(NULL, NULL) < 0 ||
        playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false,
                              true) < 0)
        return 0;
    playlist_start(0, 0, 0);
    return GO_TO_WPS;
}

static int delete_row_func(void)
{
    delete_one(chosen);
    deleted = true;
    return 1;
}

ROW(memo_play_row, LANG_PLAY, play_row_func);
ROW(memo_delete_row, LANG_DELETE, delete_row_func);

/* Menu goes back to the memos */
static int memo_menu_callback(int action, const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_list;
    if (this_item == &memo_menu && action == ACTION_STD_MENU)
        return ACTION_STD_CANCEL;
    return action;
}

static const struct menu_item_ex *const memo_menu_rows[] = {
    &memo_play_row, &memo_delete_row,
};
static const struct menu_callback_with_desc memo_menu_desc = {
    memo_menu_callback, ID2P(LANG_FRUIT_VOICE_MEMOS), Icon_NOICON };
static const struct menu_item_ex memo_menu = {
    MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(ARRAYLEN(memo_menu_rows)),
    { (void *)memo_menu_rows }, { .callback_and_desc = &memo_menu_desc } };

/* Select on a memo: Play or Delete. Delete goes back to the memos, or to
 * the main menu once Voice Memos has nothing left to show. */
static int open_memo(void)
{
    if (chosen >= listed)
        return 0;
    if (play_chosen)
    {
        play_chosen = false;
        return play_row_func();
    }
    deleted = false;
    int selected = 0;
    int ret = do_menu(&memo_menu, &selected, NULL, false);
    if (deleted)
        return shown() ? 0 : GO_TO_ROOT;
    /* Menu, or a row that ends the menu, comes back to the memos */
    return ret == GO_TO_PREVIOUS || ret == 1 ? 0 : ret;
}

/** Delete All's question **/

#define ASK_X           22
#define ASK_WIDTH       276
#define ASK_BASELINE    56
#define ASK_PITCH       18
#define DETAIL_GAP      23
#define DETAIL_PITCH    15
#define PANEL_X         12
#define PANEL_Y         175
#define PANEL_RIGHT     295
#define BUTTON_Y        188
#define BUTTON_PAD      16
#define LABEL_BASELINE  207
#define ASK_LINES       4
#define BAR_ROWS        20      /* the white status bar */

struct ask {
    int ask_font, detail_font, label_font;
    const char *lines[2 * ASK_LINES];
    int lens[2 * ASK_LINES];
    int ask_lines, detail_lines;
    int delete_width, cancel_width;
    bool yes;
};

static int line_width(const char *text, int len, int font)
{
    char buf[128];
    int w;
    snprintf(buf, sizeof(buf), "%.*s", len, text);
    font_getstringsize((const unsigned char *)buf, &w, NULL, font);
    return w;
}

/* Break `text` into lines `width` wide at spaces */
static int wrap(const char *text, int font, int width, const char **lines,
                int *lens, int most)
{
    int n = 0;
    while (*text && n < most)
    {
        int fit = strlen(text);
        while (line_width(text, fit, font) > width)
        {
            int cut = fit - 1;
            while (cut > 0 && text[cut] != ' ')
                cut--;
            if (cut <= 0)
                break;      /* one word wider than the line */
            fit = cut;
        }
        lines[n] = text;
        lens[n++] = fit;
        text += fit;
        while (*text == ' ')
            text++;
    }
    return n;
}

static int text_width(int font, const char *text)
{
    int w = 0;
    if (font >= 0)
        font_getstringsize((const unsigned char *)text, &w, NULL, font);
    return w;
}

static void put_text_part(struct stock_canvas *c, int font, const char *text,
                     int len, int x, int baseline, uint32_t rgb)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%.*s", len, text);
    if (font >= 0)
        stock_canvas_text(c, font, buf, x, baseline - font_get(font)->ascent,
                          rgb);
}

static const int grey_button[3] = {
    STOCK_ART_DIALOG_BUTTONLEFTCAP, STOCK_ART_DIALOG_BUTTONFILL,
    STOCK_ART_DIALOG_BUTTONRIGHTCAP,
};
static const int blue_button[3] = {
    STOCK_ART_DIALOG_ACTIVEBUTTONLEFTCAP, STOCK_ART_DIALOG_ACTIVEBUTTONFILL,
    STOCK_ART_DIALOG_ACTIVEBUTTONRIGHTCAP,
};

/* A button from `x0` to `x1`: left cap, fill, right cap */
static void put_button(struct stock_canvas *c, const int art[3], int x0,
                       int x1)
{
    int cap = stock_art[art[2]].width;
    stock_art_put(c, art[0], x0, BUTTON_Y);
    for (int x = x0 + stock_art[art[0]].width; x < x1 - cap; x++)
        stock_art_put(c, art[1], x, BUTTON_Y);
    stock_art_put(c, art[2], x1 - cap, BUTTON_Y);
}

static void compose_ask(struct stock_canvas *c, const struct ask *a)
{
    for (int y = 0; y < c->height; y++)
        for (int x = 0; x < c->width; x++)
            c->pixels[y * c->width + x] = 0xffffff;

    int baseline = ASK_BASELINE;
    for (int i = 0; i < a->ask_lines; i++, baseline += ASK_PITCH)
        put_text_part(c, a->ask_font, a->lines[i], a->lens[i], ASK_X, baseline,
                 0x000000);
    baseline += DETAIL_GAP - ASK_PITCH;
    for (int i = 0; i < a->detail_lines; i++, baseline += DETAIL_PITCH)
        put_text_part(c, a->detail_font, a->lines[ASK_LINES + i],
                 a->lens[ASK_LINES + i], ASK_X, baseline, 0x000000);

    stock_art_put(c, STOCK_ART_DIALOG_PANELLEFTCAP, PANEL_X, PANEL_Y);
    int first = PANEL_X + stock_art[STOCK_ART_DIALOG_PANELLEFTCAP].width;
    int tile = stock_art[STOCK_ART_DIALOG_PANELFILL].width;
    for (int x = first; x < PANEL_RIGHT; x += tile)
        stock_art_put_part(c, STOCK_ART_DIALOG_PANELFILL, 0, 0,
                           MIN(tile, PANEL_RIGHT - x),
                           stock_art[STOCK_ART_DIALOG_PANELFILL].height,
                           x, PANEL_Y);
    stock_art_put(c, STOCK_ART_DIALOG_PANELRIGHTCAP, PANEL_RIGHT, PANEL_Y);

    /* Delete and Cancel side by side, centred; the chosen one is blue */
    int wd = a->delete_width + 2 * BUTTON_PAD;
    int wc = a->cancel_width + 2 * BUTTON_PAD;
    int x0 = (LCD_WIDTH - wd - wc) / 2, mid = x0 + wd, x1 = mid + wc;
    put_button(c, grey_button, x0, x1);
    if (a->yes)
        put_button(c, blue_button, x0, mid);
    else
        put_button(c, blue_button, mid, x1);
    const char *del = str(LANG_DELETE), *cancel = str(LANG_CANCEL_0);
    put_text_part(c, a->label_font, del, strlen(del), x0 + BUTTON_PAD,
             LABEL_BASELINE, 0xffffff);
    put_text_part(c, a->label_font, cancel, strlen(cancel), mid + BUTTON_PAD,
             LABEL_BASELINE, 0xffffff);
}

/* Everything under the status bar */
static void paint_ask(const struct ask *a)
{
    uint32_t *band = extras_band(0);
    if (!band)
        return;
    const struct ui_rect body = { 0, BAR_ROWS, LCD_WIDTH,
                                  LCD_HEIGHT - BAR_ROWS };
    for (int y = BAR_ROWS; y < LCD_HEIGHT; y += BAND_ROWS)
    {
        struct stock_canvas c = { band, 0, y, LCD_WIDTH,
                                  MIN(BAND_ROWS, LCD_HEIGHT - y) };
        compose_ask(&c, a);
        stock_canvas_show(&c, body);
    }
    lcd_update_rect(0, BAR_ROWS, LCD_WIDTH,
                    LCD_HEIGHT - BAR_ROWS);
}

/* The wheel and the previous and next buttons move between the two */
static const struct button_mapping ask_map[] = {
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,               BUTTON_NONE },
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,                BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_STD_PREV,       BUTTON_LEFT|BUTTON_REL,           BUTTON_LEFT },
    { ACTION_STD_NEXT,       BUTTON_RIGHT|BUTTON_REL,          BUTTON_RIGHT },
    { ACTION_STD_OK,         BUTTON_SELECT|BUTTON_REL,         BUTTON_SELECT },
    { ACTION_STD_CANCEL,     BUTTON_MENU|BUTTON_REL,           BUTTON_MENU },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping *get_ask_map(int context)
{
    (void)context;
    return ask_map;
}

/* Stock's question page: true for Delete, false for Cancel or Menu */
static int ask_delete_all(void)
{
    struct ask a = { .yes = false };
    a.ask_font = font_load_ex(FONT_DIR "/17-helvetica-bold.pbf", 0, 64);
    a.detail_font = font_load_ex(FONT_DIR "/13-helvetica.pbf", 0, 64);
    a.label_font = font_load_ex(FONT_DIR "/13-helvetica-bold.pbf", 0, 16);
    if (a.ask_font >= 0)
        a.ask_lines = wrap(str(LANG_FRUIT_DELETE_ALL_MEMOS_ASK), a.ask_font,
                           ASK_WIDTH, a.lines, a.lens, ASK_LINES);
    if (a.detail_font >= 0)
        a.detail_lines = wrap(str(LANG_FRUIT_DELETE_ALL_MEMOS_DETAIL),
                              a.detail_font, ASK_WIDTH, a.lines + ASK_LINES,
                              a.lens + ASK_LINES, ASK_LINES);
    a.delete_width = text_width(a.label_font, str(LANG_DELETE));
    a.cancel_width = text_width(a.label_font, str(LANG_CANCEL_0));

    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "voice-memos-ask-push");
    sb_set_persistent_title(str(LANG_FRUIT_DELETE_ALL_MEMOS), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    int ret = 0;
    while (true)
    {
        paint_ask(&a);
        int action = get_custom_action(CONTEXT_PLUGIN | CONTEXT_CUSTOM, HZ / 2,
                                       get_ask_map);
        if (action == ACTION_STD_PREV || action == ACTION_STD_NEXT)
            a.yes = !a.yes;
        else if (action == ACTION_STD_OK)
        {
            ret = a.yes;
            break;
        }
        else if (action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = MENU_ATTACHED_USB;
            break;
        }
    }
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    ui_scene_close_modal(&scene, ret != MENU_ATTACHED_USB,
                         "voice-memos-ask-pop");
    font_unload(a.ask_font);
    font_unload(a.detail_font);
    font_unload(a.label_font);
    extras_bar_release();
    return ret;
}

/** The menu **/

static int start_recording(void)
{
    return mic() ? record() : 0;
}

/* Delete All, after the question: back to the main menu when Voice Memos
 * has nothing left to show */
static int delete_all_func(void)
{
    int answer = ask_delete_all();
    if (answer == MENU_ATTACHED_USB)
        return MENU_ATTACHED_USB;
    if (answer)
        delete_all();
    return shown() ? 0 : GO_TO_ROOT;
}

static int row_callback(int action, const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list);

MENUITEM_FUNCTION(start_row, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_FRUIT_START_RECORDING), start_recording,
                  row_callback, Icon_NOICON);
MENUITEM_SETTING_W_LABEL(quality_row, &global_settings.voice_memo_quality,
                         ID2P(LANG_FRUIT_QUALITY), row_callback);
MENUITEM_FUNCTION(delete_all_row, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_FRUIT_DELETE_ALL), delete_all_func, row_callback,
                  Icon_NOICON);

/* A memo's row: its date and time, from its name */
static char *memo_row_name(int index, void *data, char *buf, size_t size)
{
    (void)data;
    int i = index - FIXED_ROWS;
    if (i < 0 || i >= listed)
        return "";
    memo_label(i, buf, size);
    return buf;
}

static const struct menu_func memo_func = { .function = open_memo };
static const struct menu_get_name_and_icon memo_name_desc = {
    row_callback, memo_row_name, NULL, NULL, Icon_NOICON };
/* Every memo row is the same but for where it is */
static const struct menu_item_ex memo_rows[LISTED] = {
    [0 ... LISTED - 1] = {
        MT_FUNCTION_CALL | MENU_DYNAMIC_DESC | MENU_FUNC_CHECK_RETVAL,
        { .function = &memo_func },
        { .menu_get_name_and_icon = &memo_name_desc } },
};

bool voice_memos_describe(const struct menu_item_ex *item,
                          enum list_row_kind *kind, const char **value)
{
    if (item >= memo_rows && item < memo_rows + LISTED)
    {
        *kind = LIST_ROW_PLAIN;
        *value = NULL;
        return true;
    }
    if (item != &quality_row)
        return false;
    *kind = LIST_ROW_SETTING;
    *value = str(global_settings.voice_memo_quality ?
                 LANG_FRUIT_QUALITY_HIGH : LANG_FRUIT_QUALITY_LOW);
    return true;
}

bool voice_memos_step(const struct menu_item_ex *item)
{
    if (item != &quality_row)
        return false;
    global_settings.voice_memo_quality = !global_settings.voice_memo_quality;
    settings_save();
    return true;
}

/* Which rows show: Start Recording and Quality while the microphone is in,
 * Delete All and the memos while there are any. Play/Pause reaches the menu
 * only on a memo's row (voice_memos_plays), and plays it. */
static int row_callback(int action, const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM)
    {
        bool show;
        if (this_item == &voice_memos_menu)
            show = shown();
        else if (this_item == &start_row || this_item == &quality_row)
            show = mic();
        else if (this_item == &delete_all_row)
        {
            show = memo_count() > 0;
        }
        else
            show = this_item - memo_rows < listed;
        return show ? action : ACTION_EXIT_MENUITEM;
    }
    if (action == ACTION_ENTER_MENUITEM &&
        this_item >= memo_rows && this_item < memo_rows + LISTED)
        chosen = this_item - memo_rows;
    if (action == ACTION_TREE_WPS && this_item == &voice_memos_menu)
    {
        play_chosen = true;
        return ACTION_STD_OK;
    }
    return action;
}

#define MEMO_ROW(i)     &memo_rows[i],
#define MEMO_ROWS10(i)  MEMO_ROW(i) MEMO_ROW(i + 1) MEMO_ROW(i + 2) \
    MEMO_ROW(i + 3) MEMO_ROW(i + 4) MEMO_ROW(i + 5) MEMO_ROW(i + 6) \
    MEMO_ROW(i + 7) MEMO_ROW(i + 8) MEMO_ROW(i + 9)
#define MEMO_ROWS50(i)  MEMO_ROWS10(i) MEMO_ROWS10(i + 10) \
    MEMO_ROWS10(i + 20) MEMO_ROWS10(i + 30) MEMO_ROWS10(i + 40)

static const struct menu_item_ex *const menu_rows[] = {
    &start_row, &quality_row, &delete_all_row,
    MEMO_ROWS50(0) MEMO_ROWS50(50) MEMO_ROWS50(100) MEMO_ROWS50(150)
    MEMO_ROWS50(200)
};
static const struct menu_callback_with_desc menu_desc = {
    row_callback, ID2P(LANG_FRUIT_VOICE_MEMOS), Icon_NOICON };
const struct menu_item_ex voice_memos_menu = {
    MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(ARRAYLEN(menu_rows)),
    { (void *)menu_rows }, { .callback_and_desc = &menu_desc } };

bool voice_memos_plays(const struct menu_item_ex *menu,
                       const struct menu_item_ex *row)
{
    return menu == &voice_memos_menu &&
           row >= memo_rows && row < memo_rows + LISTED;
}
