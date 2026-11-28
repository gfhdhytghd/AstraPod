/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Fruit Company's Voice Memos, as the stock iPod Classic 2.0.5 has them.
 * The main menu shows the row, between Extras and Settings, while a headset
 * with a microphone is in the jack. Its menu starts a recording or sets its
 * Quality. The recording screen shows the memo's number, date and length;
 * Menu or Play/Pause pauses it into a menu of Resume, Stop and Save and
 * Delete, and a held Menu saves it and goes to the main menu. Memos are WAV
 * files in /Recordings named by the time they started.
 *
 * Parts: the memos on disk, the pane, the recorder, the recording screen,
 * the paused menu, the menu.
 ****************************************************************************/

#include <stdio.h>
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
#include "audio/pcm_record.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "system/shutdown.h"
#include "ui/menu_pane.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/splash.h"
#include "root_menu.h"
#include "screens/recording.h"
#include "screens/extras/extras_bar.h"
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

static int memos = -1;      /* how many, or -1 until counted */

static int memo_count(void)
{
    if (memos >= 0)
        return memos;
    memos = 0;
    DIR *dir = opendir(REC_BASE_DIR);
    if (!dir)
        return memos;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (memo_name((const char *)entry->d_name))
            memos++;
    closedir(dir);
    return memos;
}

/** The pane **/

enum state { IDLE, RECORDING, PAUSED };
static enum state state = IDLE;
static unsigned long paused_ticks;  /* the paused memo's length */

static bool mic(void)
{
    return mikey_mic_present();
}

bool voice_memos_refresh(void)
{
    static int last = -1;
    int now = mic();
    bool changed = last >= 0 && last != now;
    if (changed && now)
        memos = -1;     /* the disk may have changed while it was out */
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

bool voice_memos_owns(const struct menu_item_ex *menu)
{
    return menu == &voice_memos_menu || menu == &paused_menu;
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

/** The menu **/

static int start_recording(void)
{
    return mic() ? record() : 0;
}

MENUITEM_FUNCTION(start_row, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_FRUIT_START_RECORDING), start_recording, NULL,
                  Icon_NOICON);
MENUITEM_SETTING_W_LABEL(quality_row, &global_settings.voice_memo_quality,
                         ID2P(LANG_FRUIT_QUALITY), NULL);

bool voice_memos_describe(const struct menu_item_ex *item,
                          enum list_row_kind *kind, const char **value)
{
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

/* The main menu shows the row while the microphone is in */
static int menu_callback(int action, const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && this_item == &voice_memos_menu &&
        !mic())
        return ACTION_EXIT_MENUITEM;
    return action;
}

static const struct menu_item_ex *const memo_rows[] = {
    &start_row, &quality_row,
};
static const struct menu_callback_with_desc memo_desc = {
    menu_callback, ID2P(LANG_FRUIT_VOICE_MEMOS), Icon_NOICON };
const struct menu_item_ex voice_memos_menu = {
    MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(ARRAYLEN(memo_rows)),
    { (void *)memo_rows }, { .callback_and_desc = &memo_desc } };
