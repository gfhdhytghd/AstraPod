/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Measures every track in the library and writes the sound index.
 *
 * The whole of this screen is one loop: walk the database, decode a window of
 * each track that has no current record, and append what it measured. It runs
 * for hours and its design follows from that rather than from anything about
 * the analysis.
 *
 * Three rules fall out of the duration. It runs only on the charger, and asks
 * for one *after* explaining itself rather than before -- someone who does not
 * yet know what this is cannot be expected to have plugged in for it. Losing
 * the charger pauses it rather than ending it: a player running unpowered with
 * the disk awake is the worst state it can be in, but so is a message the user
 * cannot act on before the screen closes. And stopping never loses anything --
 * the index is appended to as it goes, so there are two endings and both keep
 * what was measured.
 *
 * While it runs the player charges without mounting (USB_MODE_CHARGE): a cable
 * found on a desk is nearly always power, and handing the disk to a laptop
 * a quarter of the way through loses the run for a connection nobody asked
 * for.
 *
 * Parts, in order:
 *   - progress and the display
 *   - the per-track callbacks the decoder polls
 *   - one track
 *   - the run
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "power.h"
#include "powermgmt.h"
#include "backlight.h"
#include "audio.h"
#include "usb.h"
#include "file.h"
#include "metadata.h"
#include "string-extra.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "audio/beat_probe.h"
#include "audio/track_decode.h"
#include "database/sound_cal.h"
#include "database/path_key.h"
#include "database/sound_index.h"
#include "database/tagcache.h"
#include "screens/system/sound_scan.h"
#include "draw/screen_access.h"
#include "widgets/splash.h"
#include "widgets/yesno.h"
#include "widgets/dialog.h"
#include "widgets/dialog_prose.h"
#include "draw/viewport.h"
#include "font.h"
#include "input/action.h"
#include "api/misc.h"
#include "system/format_time.h"
#include "ui/frame.h"
#include "ui/scene.h"
#include "ui/sheet.h"

#define SCAN_PANEL_PIXELS (LCD_WIDTH * LCD_HEIGHT)

struct scan_panel {
    struct ui_sheet sheet;
    struct ui_rect bounds;
    int handle;
};

static void ss_open_panel(struct scan_panel *panel)
{
    panel->handle = core_alloc_free_space(2 * FRAMEBUFFER_SIZE);
    if (panel->handle <= 0)
        return;
    ui_scene_interrupt();
    fb_data *pixels = core_get_data_pinned(panel->handle);
    memcpy(pixels, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
    core_put_data_pinned(pixels);
    panel->sheet.light = UI_FULL_OPACITY;
}

static void ss_close_panel(struct scan_panel *panel, bool animate)
{
    if (panel->handle <= 0)
        return;
    if (animate && panel->bounds.width > 0)
    {
        fb_data *pixels = core_get_data_pinned(panel->handle);
        bool classic = dialog_is_classic();
        panel->sheet.zoom = classic;
        ui_sheet_begin(&panel->sheet, false, "sound-scan-progress");
        do {
            if (classic)
                ui_sheet_paint_zoom(&panel->sheet, pixels,
                                    pixels + SCAN_PANEL_PIXELS, panel->bounds, 0);
            else
                ui_sheet_paint(&panel->sheet, pixels, pixels + SCAN_PANEL_PIXELS,
                               panel->bounds, false);
        } while (ui_sheet_wait(&panel->sheet));
        if (panel->sheet.light != UI_FULL_OPACITY || panel->sheet.previous.height > 0)
        {
            memcpy(lcd_framebuffer_default.fb_ptr, pixels, FRAMEBUFFER_SIZE);
            ui_mark_dirty(SCREEN_MAIN);
            ui_flush_dirty();
        }
        core_put_data_pinned(pixels);
    }
    ui_sheet_cancel(&panel->sheet);
    panel->handle = core_free(panel->handle);
}

/* The window, from the specification: past the intro, and long enough for the
 * tempo envelope to fill and then hold.
 *
 * The whole window is taken. Stopping as soon as beat_probe_settled() is true
 * ended the decode at around eleven seconds, and that is a tempo criterion
 * ending every other measurement with it -- loudness, the band levels, the
 * onset rates and the chroma all stop being sampled because the *tempo* has
 * converged. Measured over thirty tracks against a forty-second window, the
 * eleven-second one disagreed about the nearest track two times in three,
 * changed its mind about whether to trust the tempo on thirteen of them and
 * about major-versus-minor on twelve. Those axes carry most of the weight in
 * a match, so the early stop was buying scan time with the thing the scan is
 * for. */
#define SS_START_PCT     15
#define SS_START_MIN_MS  5000
#define SS_START_MAX_MS  30000
#define SS_WINDOW_MS     40000

/* The codec's view of the file. Must exceed the largest contiguous read any
 * codec asks for, or track_decode.c re-centres its window on every request. */
#define SS_WINDOW_BYTES  (256 * 1024)

/* Tracks too short to measure. The tempo envelope alone wants seventeen
 * seconds and the window starts after the intro. */
#define SS_MIN_LENGTH_MS 25000

static int           ss_handle;

static int           ss_total;
static int           ss_live;      /* Of those, not flagged deleted. 0 =
                                      could not be counted */
static int           ss_done;      /* Measured this run */
static int           ss_skipped;   /* Already current */
static int           ss_failed;

static long          ss_work;      /* Ticks spent measuring */
static unsigned long ss_audio_ms;  /* Audio measured, for the rate */

static char          ss_now[64];

static bool          ss_stop;      /* The user asked */
static bool          ss_unplugged;
static int           ss_event;     /* System event ss_abort() took, or 0 */


/** Progress **/

/* What the run is asking for, before it starts.
 *
 * Long enough to need scrolling, which is the point: three of these four
 * paragraphs are things a user would be annoyed to discover afterwards, and
 * the fourth is the one that makes the other three acceptable. */
static const char ss_explain[] =
    "This measures how every track sounds -- its tempo, loudness, energy "
    "and key -- and stores the result, so playlists can be built from how "
    "the music feels rather than from its tags.\n"
    "\n"
    "It takes hours. A few thousand tracks is most of a night.\n"
    "\n"
    "The player has to stay plugged in, and stops if the charger is "
    "removed. Nothing else runs while it works: the disk stays awake "
    "throughout and the processor runs flat out, so the player will be warm "
    "and will do nothing else until it has finished.\n"
    "\n"
    "Stopping costs nothing -- you can update the analysis, which will carry "
    "on from where it left off.";

static const char ss_depth_explain[] =
    "Thorough listens to forty seconds of each track. Quick stops as soon as "
    "it has worked out the tempo, around eleven seconds, and gets through the "
    "library about three and a half times faster.\n"
    "\n"
    "Quick is faster than it is accurate. Working out the tempo is what stops "
    "it, and that ends the measurement of loudness, tone and key along with "
    "it -- measured against a full analysis, Quick picked a different closest "
    "track two times in three.\n"
    "\n"
    "Choose Quick if a full analysis on the player is too long to sit "
    "through. The desktop tool does a whole library thoroughly in minutes.";

/* Asked once the run has been agreed rather than before it: this is a
 * question about how long to spend, which only matters to somebody who has
 * decided to spend it. Remembered, so the answer also settles what a later
 * update does.
 *
 * It opens on whatever the setting of the same name currently says. The
 * Playlist Engine menu carries that setting too, so a dialog that always
 * opened on Thorough would offer somebody who had chosen Quick the opposite
 * of their own answer, and overwrite it if they took the highlighted one. */
static void ss_ask_depth(void)
{
    bool thorough = global_settings.analysis_depth != ANALYSIS_QUICK;

    global_settings.analysis_depth =
        dialog_prose_confirm("Analysis depth", ss_depth_explain,
                             "Thorough", "Quick", thorough)
        ? ANALYSIS_THOROUGH : ANALYSIS_QUICK;

    settings_save();
}

/* The running display: a framed box with the figures and one button.
 *
 * A box rather than the whole screen because that is what it is -- one modal
 * thing happening over the player, with one way out. The scan owns its
 * controller because it polls for input inside the decoder. */
/* Four lines and a button, in that order: what it is doing, what it is doing
 * it to, how long that leaves, and the bar. The pause takes the same four so
 * the box does not resize when the charger comes out. */
static void ss_draw(struct scan_panel *panel, const char *paused)
{
    struct screen *lcd = &screens[SCREEN_MAIN];
    struct dialog_style style;
    dialog_style_glass(&style);
    bool classic = dialog_is_classic();
    struct viewport vp, content, *last_vp;
    char line[64];
    int fh, bh;
    int y, w, bar;
    fb_data *pixels = NULL;
    struct frame_buffer_t tile;
    bool opening = panel->bounds.width <= 0;

    viewport_set_defaults(&vp, lcd->screen_type);
    last_vp = lcd->set_viewport(&vp);

    fh = font_get(vp.font)->height;
    bh = fh + 8;

    vp.width  = lcd->lcdwidth * 92 / 100;
    vp.height = fh * 4 + fh / 2 + bh + style.box_margin * 2 + 8;
    if (vp.height > lcd->lcdheight)
        vp.height = lcd->lcdheight;
    vp.x = (lcd->lcdwidth - vp.width) / 2;
    vp.y = (lcd->lcdheight - vp.height) / 2;

    if (panel->handle > 0)
    {
        pixels = core_get_data_pinned(panel->handle);
        if (classic)
        {
            ui_sheet_dim(pixels + SCAN_PANEL_PIXELS, pixels, SCAN_PANEL_PIXELS);
            int sx = MAX(0, vp.x - 8), sy = MAX(0, vp.y - 6);
            int sx2 = MIN(LCD_WIDTH, vp.x + vp.width + 8);
            int sy2 = MIN(LCD_HEIGHT, vp.y + vp.height + 10);
            panel->bounds = (struct ui_rect){
                sx, sy, MAX(0, sx2 - sx), MAX(0, sy2 - sy)
            };
            tile = (struct frame_buffer_t){
                .fb_ptr = pixels + SCAN_PANEL_PIXELS,
                .stride = LCD_WIDTH,
                .elems = SCAN_PANEL_PIXELS,
            };
        }
        else
        {
            panel->bounds = (struct ui_rect){vp.x, vp.y, vp.width, vp.height};
            tile = (struct frame_buffer_t){ .fb_ptr = pixels + SCAN_PANEL_PIXELS,
                                            .stride = vp.width,
                                            .elems = (size_t)vp.width * vp.height };
            vp.x = vp.y = 0;
        }
        vp.buffer = &tile;
        lcd->set_viewport(&vp);
    }

    dialog_frame_box(lcd, &vp, &style, &content);
    lcd->set_viewport(&content);
    lcd->set_drawmode(DRMODE_FG);

    /* No heading. The user chose this a moment ago and read a screen of prose
     * about it; a line saying what it is would be the only thing here that
     * tells them nothing. */
    y = 0;

    if (paused != NULL)
    {
        lcd->putsxy(0, y, "Paused");
        y += fh;
        lcd->putsxy(0, y, paused);
        y += fh;

        /* The third line stays empty: there is no rate while nothing is
         * being measured, and an estimate left over from before the pause
         * would say the run was still going. */
        y += fh;
    }
    else
    {
        snprintf(line, sizeof (line), "Analysing %d of %d",
                 ss_done + ss_skipped, ss_total);
        lcd->putsxy(0, y, line);
        y += fh;

        /* Clipped rather than wrapped -- the line below it is load bearing
         * and a two-line name would push it out of the box. */
        lcd->putsxy(0, y, ss_now);
        y += fh;

        /* Only once a track has finished: before that the estimate is made
         * of one sample and jumps about. */
        if (ss_done > 0 && ss_work > 0)
        {
            int left = ss_total - ss_done - ss_skipped;
            long eta = (ss_work / ss_done) * (left > 0 ? left : 0) / HZ;

            if (eta >= 3600)
                snprintf(line, sizeof (line), "About %ldh %ldm left",
                         eta / 3600, (eta % 3600) / 60);
            else
                snprintf(line, sizeof (line), "About %ldm left",
                         (eta + 59) / 60);

            lcd->putsxy(0, y, line);
        }
        else
        {
            lcd->putsxy(0, y, "Working out how long...");
        }

        y += fh;
    }

    y += fh / 4;

    /* The bar is the one thing readable at arm's length, and it keeps
     * running through a pause -- what has been done is still done. */
    w = content.width;
    if (classic)
        dialog_draw_progress(lcd, 0, y, w, MIN(fh - 2, 14),
                             ss_done + ss_skipped, ss_total);
    else
    {
        bar = ss_total > 0 ? (ss_done + ss_skipped) * w / ss_total : 0;
        lcd->set_drawmode(DRMODE_SOLID);
        lcd->drawrect(0, y, w, fh - 2);
        lcd->fillrect(1, y + 1, bar > 2 ? bar - 2 : 0, fh - 4);
    }

    /* One button, and it is the only thing this screen does. */
    dialog_draw_button(lcd, &style, content.width / 4,
                       content.height - bh, content.width / 2, bh,
                       "Stop", true);

    lcd->set_viewport(last_vp);
    if (pixels)
    {
        if (opening)
        {
            panel->sheet.zoom = classic;
            ui_sheet_begin(&panel->sheet, true, "sound-scan-progress");
        }
        if (classic)
        {
            panel->sheet.light = -1;
            ui_sheet_paint_zoom(&panel->sheet, pixels, tile.fb_ptr,
                                panel->bounds, 0);
            while (ui_sheet_wait(&panel->sheet))
                ui_sheet_paint_zoom(&panel->sheet, pixels, tile.fb_ptr,
                                    panel->bounds, 0);
            if (panel->sheet.active)
            {
                ui_sheet_cancel(&panel->sheet);
                panel->sheet.light = -1;
                ui_sheet_paint_zoom(&panel->sheet, pixels, tile.fb_ptr,
                                    panel->bounds, 0);
            }
        }
        else
        {
            ui_sheet_paint(&panel->sheet, pixels, tile.fb_ptr, panel->bounds, true);
            while (ui_sheet_wait(&panel->sheet))
                ui_sheet_paint(&panel->sheet, pixels, tile.fb_ptr, panel->bounds, false);
            if (panel->sheet.active)
            {
                ui_sheet_cancel(&panel->sheet);
                ui_sheet_paint(&panel->sheet, pixels, tile.fb_ptr, panel->bounds, false);
            }
        }
        core_put_data_pinned(pixels);
    }
    else
        lcd->update_rect(vp.x, vp.y, vp.width, vp.height);
}


/** What the decoder polls **/

/* Called from inside the codec, often. The charger is checked here rather
 * than once a track: a slow track takes tens of seconds, and that is a long
 * time to keep running unpowered with the disk awake. */
static bool ss_abort(void)
{
    int button;

    if (!charger_inserted())
    {
        ss_unplugged = true;
        return true;
    }

    button = get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);

    /* SELECT as well as the two back keys: the screen draws one button and
     * draws it selected, so pressing select has to be what it looks like. */
    if (button == ACTION_STD_OK || button == ACTION_STD_CANCEL ||
        button == ACTION_STD_MENU)
    {
        ss_stop = true;
        return true;
    }

    /* Handled once the walk has closed the index; until then every thread
     * waits on a USB connection, and a shutdown would leave the index open. */
    if (button == SYS_USB_CONNECTED || button == SYS_POWEROFF ||
        button == SYS_REBOOT)
    {
        ss_event = button;
        ss_stop = true;
        return true;
    }

    return false;
}


/** One track **/

/* The file's length, for the staleness check. The decoder reports this too,
 * but the point of the check is to decide whether to decode at all. */
static uint32_t ss_file_size(const char *path)
{
    int fd = open(path, O_RDONLY);
    off_t n;

    if (fd < 0)
        return 0;

    n = ffilesize(fd);
    close(fd);

    return n > 0 ? (uint32_t)n : 0;
}

/* Measure 'path' and append it. False only if the run should end. */
static bool ss_measure(const char *path, uint32_t mtime, uint32_t genre_key,
                       int year, unsigned long length_ms, void *buf)
{
    struct track_sound s;
    struct sound_record rec;
    unsigned long start_ms, analysed = 0;
    uint64_t key = path_key(path);
    uint32_t size = ss_file_size(path);
    long t0;
    int rc;

    if (sound_index_done(key, mtime, size))
    {
        ss_skipped++;
        return true;
    }

    start_ms = length_ms / 100 * SS_START_PCT;
    if (start_ms < SS_START_MIN_MS)
        start_ms = SS_START_MIN_MS;
    if (start_ms > SS_START_MAX_MS)
        start_ms = SS_START_MAX_MS;
    if (length_ms < start_ms + 5000)
        start_ms = 0;

    beat_probe_start();

    t0 = current_tick;
    /* Quick hands the decode a reason to stop early; Thorough gives it none
     * and takes the whole window. */
    rc = track_decode_run(path, start_ms, SS_WINDOW_MS, buf, SS_WINDOW_BYTES,
                          beat_probe_sink,
                          global_settings.analysis_depth == ANALYSIS_QUICK
                          ? beat_probe_settled : NULL,
                          ss_abort, &analysed);
    ss_work += current_tick - t0;

    if (ss_stop || ss_unplugged)
        return false;

    /* No record: the file was never tried, and the next run measures it. */
    if (rc == TRACK_DECODE_NO_THREAD)
    {
        ss_skipped++;
        return true;
    }

    beat_probe_result(&s);
    sound_index_fill(&rec, key, mtime, size, genre_key, year, &s, rc);

    /* A file that will not decode still gets a record, so the next run does
     * not spend the same seconds discovering the same thing. */
    if (rc == TRACK_DECODE_FAILED || rc == TRACK_DECODE_NO_CODEC ||
        rc == TRACK_DECODE_NO_FILE)
    {
        ss_failed++;
    }

    if (analysed < SS_MIN_LENGTH_MS / 2)
        rec.flags |= SOUND_F_SHORT;

    ss_audio_ms += analysed;
    ss_done++;

    return sound_index_add(&rec);
}


/** The run **/

/* Wait for power, with the screen still up, rather than giving up on the run.
 *
 * Saying "Connect to charger" and then closing asks the user to go and find a
 * cable and afterwards start again from the menu -- and the message is gone
 * by the time they have. Waiting costs nothing and is the same answer to both
 * cases: no charger at the start, and a charger pulled out half way through.
 * The second is a pause, not an abandonment; the run has lost nothing but the
 * track it was in the middle of.
 *
 * False if the user stopped instead of plugging in. */
static bool ss_wait_for_charger(struct scan_panel *panel)
{
    if (!charger_inserted() && global_settings.talk_menu)
        talk_text("Connect the charger to continue", false);
    while (!charger_inserted())
    {
        int button;

        ss_draw(panel, "Connect the charger to continue");
        reset_poweroff_timer();

        button = get_action(CONTEXT_STD, HZ / 2);

        if (button == ACTION_STD_OK || button == ACTION_STD_CANCEL ||
            button == ACTION_STD_MENU)
        {
            ss_stop = true;
            return false;
        }

        if (button == SYS_USB_CONNECTED)
            ss_close_panel(panel, false);
        if (default_event_handler(button) == SYS_USB_CONNECTED)
        {
            ss_stop = true;
            return false;
        }
    }

    ss_unplugged = false;

    return true;
}

/* Whether a finished index is already there to be thrown away. */
static bool ss_have_index(void)
{
    struct sound_index_reader r;

    if (sound_index_reader_open(&r) != SOUND_OK)
        return false;

    sound_index_reader_close(&r);

    return true;
}

static bool ss_gate(bool *fresh)
{
    int partial = 0;
    bool update = false;

    if (!tagcache_is_usable())
    {
        splash(HZ * 3, "Needs the database");
        return false;
    }

    /* A part-finished run outranks whichever row was chosen. Discarding it is
     * a decision made here, in front of the number, rather than a side-effect
     * of picking the rebuild row.
     *
     * yesno_pop() rather than yesno_pop_confirm(): the latter puts "Are you
     * sure?" on a line of its own above whatever it is given, which reads as
     * two questions when the thing below is already one. */
    if (sound_index_partial(&partial, &update))
    {
        char q[64];

        snprintf(q, sizeof (q),
                 "An analysis is part finished (%d done). Carry on with it?",
                 partial);

        if (yesno_pop(q))
        {
            *fresh = false;
            return true;
        }

        if (!yesno_pop("Start again from the beginning?"))
            return false;

        /* Again as the same kind of run: an abandoned update must not turn
         * into a full rebuild. */
        if (update)
        {
            sound_index_discard_part();
            *fresh = false;
        }
        else
            *fresh = true;
        return true;
    }

    if (!dialog_prose_confirm("Analyse library", ss_explain,
                              "Start", "Not now", true))
    {
        return false;
    }

    ss_ask_depth();

    /* Asked last, and only when it is a real question: there is a finished
     * index and the row picked was the one that throws it away. Measuring a
     * library again is a night, so the cheaper answer is offered rather than
     * just refused. */
    if (*fresh && ss_have_index())
    {
        if (yesno_pop("You already have an index. Rebuild it from scratch?"))
            return true;

        if (!yesno_pop("Update it instead?"))
            return false;

        *fresh = false;
    }

    return true;
}

static void ss_result_measure(struct dialog *d, struct screen *screen,
                              struct viewport *box, void *data)
{
    struct dialog_insets insets;
    int width, height;
    dialog_get_insets(&d->style, &insets);
    screen->getstringsize(data, &width, &height);
    box->width = MIN(screen->lcdwidth, width + insets.left + insets.right);
    box->height = MIN(screen->lcdheight, height + insets.top + insets.bottom);
    box->x = (screen->lcdwidth - box->width) / 2;
    box->y = (screen->lcdheight - box->height) / 2;
}

static void ss_result_draw(struct dialog *d, struct screen *screen,
                           struct viewport *content, void *data)
{
    (void)d;
    content->flags |= VP_FLAG_ALIGN_CENTER;
    screen->set_viewport(content);
    screen->set_drawmode(DRMODE_FG);
    screen->putsxy(0, 0, data);
}

static int ss_result_action(struct dialog *d, int button, void *data)
{
    (void)d;
    (void)data;
    if (IS_SYSEVENT(button))
        return default_event_handler(button) == SYS_USB_CONNECTED
               ? DIALOG_ABORT : DIALOG_CONTINUE;
    return button == ACTION_NONE || button == ACTION_REDRAW
           ? DIALOG_CONTINUE : DIALOG_ACCEPT;
}

/* Keep the result up until someone returns to the player and presses a key.
 * A long run can finish while the player is unattended. */
static void ss_result(const char *msg)
{
    static const struct dialog_callbacks callbacks = {
        .measure = ss_result_measure,
        .draw = ss_result_draw,
        .on_action = ss_result_action,
    };
    struct dialog dialog;
    dialog_init(&dialog, CONTEXT_STD, NULL, NULL, &callbacks, (void *)msg);
    dialog.glass = true;
    dialog.motion_route = "sound-scan-result";
    backlight_on();
    if (global_settings.talk_menu)
        talk_string(msg, false);
    dialog_run(&dialog, TIMEOUT_BLOCK);
}

static void ss_run(bool fresh)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    char msg[64];
    int index_size;
    bool complete;
    int rc;
    struct scan_panel panel = {0};

    audio_stop();

    /* Charge without mounting, for the length of the run. A cable found on a
     * desk is nearly always power, and a scan that gets a quarter of the way
     * through and then hands the disk to a laptop has lost the run for the
     * sake of a connection nobody asked for. Restored on every way out,
     * including the setup failures below: a mode left behind is a player that
     * charges from a computer and never appears on it, with nothing on screen
     * to say why and a reboot the only way back. */
    usb_set_mode(USB_MODE_CHARGE);

    ss_total = tagcache_get_stat()->total_entries;

    /* What the walk will actually yield, which is the total less the entries
     * flagged deleted -- taken here, beside the total it is derived from,
     * rather than at the end where a deletion made meanwhile would move it.
     * Zero where they cannot be counted; see the pruning decision below. */
    {
        struct tagcache_marks marks;

        tagcache_get_marks(&marks);
        ss_live = marks.deleted_ct >= 0 && marks.deleted_ct <= ss_total
                  ? ss_total - marks.deleted_ct : 0;
    }
    /* The total sizes the index; what the screen counts to is what the walk
     * yields, or the counter stops short of it on any library that has lost
     * a track and the run looks stuck at the end. */
    index_size = ss_total;
    if (ss_live > 0)
        ss_total = ss_live;

    ss_done = ss_skipped = ss_failed = 0;
    ss_work = 0;
    ss_audio_ms = 0;
    ss_stop = ss_unplugged = false;
    ss_event = 0;
    strlcpy(ss_now, "", sizeof (ss_now));

    /* Immovable. The window's address is handed to track_decode.c, which
     * holds it for the length of a decode -- tens of seconds inside a codec,
     * with a file read behind every callback. A movable block is one core
     * allocation anywhere from moving out from under that. */
    ss_handle = core_alloc_ex(SS_WINDOW_BYTES, &buflib_ops_locked);
    if (ss_handle <= 0)
    {
        usb_set_mode(global_settings.usb_mode);
        splash(HZ * 3, "Not enough memory");
        return;
    }

    rc = sound_index_begin(index_size + 1, fresh);
    if (rc != SOUND_OK)
    {
        core_free(ss_handle);
        usb_set_mode(global_settings.usb_mode);
        splash(HZ * 3, "Could not open the index");
        return;
    }

    /* Asked for after the explanation, not before it: someone who does not
     * yet know what this is cannot be expected to have plugged in for it. The
     * screen is already up, so the request has somewhere to live and the run
     * starts the moment the cable goes in. */
    ss_open_panel(&panel);
    if (!ss_wait_for_charger(&panel))
    {
        ss_close_panel(&panel, true);
        sound_index_close();
        core_free(ss_handle);
        usb_set_mode(global_settings.usb_mode);
        return;
    }

    ss_draw(&panel, NULL);

    if (!tagcache_search(&tcs, tag_filename))
    {
        ss_close_panel(&panel, true);
        sound_index_close();
        core_free(ss_handle);
        usb_set_mode(global_settings.usb_mode);
        splash(HZ * 3, "Database busy");
        return;
    }

    while (tagcache_get_next(&tcs, path, sizeof (path)))
    {
        char genre[48];
        unsigned long length_ms;
        uint32_t mtime;
        int year;

        /* No button is pressed for hours, so idle poweroff would otherwise
         * fire in the middle of the run. */
        reset_poweroff_timer();

        /* Before the length test below, not after it. What this marks is that
         * the track still exists, which is true of one too short to measure;
         * marking only the measured ones would prune every short track's
         * record on the first run that finished. */
        sound_index_seen(path_key(path));

        length_ms = (unsigned long)tagcache_get_numeric(&tcs, tag_length);
        mtime = (uint32_t)tagcache_get_numeric(&tcs, tag_mtime);
        year = (int)tagcache_get_numeric(&tcs, tag_year);

        genre[0] = '\0';
        tagcache_retrieve(&tcs, tcs.idx_id, tag_genre, genre, sizeof (genre));

        strlcpy(ss_now, strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                sizeof (ss_now));

        if (length_ms < SS_MIN_LENGTH_MS)
        {
            ss_skipped++;
        }
        else
        {
            /* Retried rather than skipped when the charger goes: the track
             * was abandoned mid-decode and measured nothing, and the walk has
             * already moved past it. Waiting and measuring it again is the
             * only way it gets done in this run. */
            while (!ss_measure(path, mtime, sound_index_genre_key(genre),
                               year, length_ms, core_get_data(ss_handle)))
            {
                if (ss_stop || !ss_unplugged)
                    break;

                if (!ss_wait_for_charger(&panel))
                    break;
            }
        }

        if (ss_stop)
            break;

        /* Once a track, not on a timer: at seconds a track there is nothing
         * to animate, and the backlight is off for most of the run anyway. */
        ss_draw(&panel, NULL);
    }

    tagcache_search_finish(&tcs);
    core_free(ss_handle);
    usb_set_mode(global_settings.usb_mode);

    /* Whether the walk reached every track the database holds, which is what
     * lets finish() drop the records of tracks that have left the player.
     *
     * A count rather than "the loop ended": tagcache_get_next() returns false
     * at the end of a walk and also when a read fails or the database goes
     * unreadable underneath one, and taking the second for the first throws
     * away the measurements of everything it had not reached yet.
     *
     * Counted against the live entries, not total_entries. delete_entry()
     * flags an entry and leaves the count alone (tagcache.c says so), and the
     * walk skips what it flagged -- so on any library a track has ever left,
     * comparing against the total can never be satisfied, and the pruning
     * would silently never happen on exactly the libraries that need it.
     *
     * ss_live is zero where the deleted entries cannot be counted, which is
     * off the ramcache. Nothing is pruned then; that is the safe answer to
     * not knowing. */
    complete = ss_live > 0 && ss_done + ss_skipped >= ss_live;

    /* Losing the charger is no longer an ending -- the run waits for it and
     * carries on -- so there are two: the user stopped, or it finished. */
    if (ss_stop)
    {
        ss_close_panel(&panel, !ss_event);
        sound_index_close();
        if (ss_event)
            default_event_handler(ss_event);
        else
            splashf(HZ * 4, "Stopped. %d of %d done",
                    ss_done + ss_skipped, ss_total);
        return;
    }

    /* Saving and calibrating take a while on a large index, and the box
     * would otherwise sit on the last track's name as if nothing moved. */
    strlcpy(ss_now, "Saving the results...", sizeof (ss_now));
    ss_draw(&panel, NULL);

    if (sound_index_finish(complete) == SOUND_OK)
    {
        /* Here rather than on first use, where sound_cal_ensure() would
         * otherwise reach it: this is a screen the user is already waiting
         * on, and a pass over the index it has just written is a second
         * against the hours behind it. */
        sound_cal_update();
        ss_close_panel(&panel, true);

        snprintf(msg, sizeof (msg), "Done. %d measured, %d unreadable",
                 ss_done - ss_failed, ss_failed);
        ss_result(msg);
    }
    else
    {
        sound_index_close();
        ss_close_panel(&panel, true);
        ss_result("Could not write the index");
    }
}

bool sound_scan_screen(bool rebuild)
{
    bool fresh = rebuild;

    if (!ss_gate(&fresh))
        return false;

    /* Off for the run: the theme's status bar is redrawn on every button the
     * run reads, over the box, which then stays gone until the next track
     * ends. It would also show the depth question's title, which the dialog
     * leaves behind. */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    ss_run(fresh);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, true);
    return true;
}
