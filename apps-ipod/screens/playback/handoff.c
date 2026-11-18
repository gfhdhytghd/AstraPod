/* PodBox continuity handoff. GNU General Public License, version 2+. */
#include "handoff.h"
#ifdef HAVE_USB_PODLINK
#include "audio.h"
#include "audio/pcmbuf.h"
#include "button.h"
#include "cider.h"
#include "file.h"
#include "input/action.h"
#include "kernel.h"
#include "lang.h"
#include "metadata.h"
#include "pcm_mixer.h"
#include "system/activity.h"
#include "system/shutdown.h"
#include "usb.h"
#include "widgets/dialog.h"
#include "handoff_sheet.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACK_FIELD 97
#define FADE_TICKS (3 * HZ)
#define MATCH_TICKS (5 * HZ)
#define START_TICKS (8 * HZ)
#define ALIGN_LIMIT_MS 1000
#define LOG_PATH "/.rockbox/handoff-log.txt"

enum phase { IDLE, MATCH, QUESTION, PREPARE, ARMED, WAIT_END, COMPLETE, IGNORE };
enum reply { NONE, OFFER, FAIL, TIMEOUT, INVALID_GO, ALIGNMENT };
enum transport { CLOSED, SYNCING, ACCEPTED, SCHEDULED, VERIFIED };

static struct mutex wire;
static char outgoing[3 * TRACK_FIELD + 48];
static char ping[20];
static long ping_received;
static char ping_sequence;
static enum reply reply;
static char reply_args[16];
static enum transport transport;
static volatile bool capable;
static volatile unsigned int link_epoch;
static unsigned int ui_epoch;
static enum phase phase; /* UI thread only */
static unsigned long ident, deadline, start, fade_begin, fade_end;
static unsigned long rtt, startup, uncertainty, duration;
static long offset, error, projected;
static bool rufin, remote_finished, sheet_open, dissolve;
static struct handoff_sheet sheet;
static long failure_until;
static uint32_t path_crc;

static bool playing(void)
{
    return (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) ==
        AUDIO_STATUS_PLAY;
}

bool handoff_capable(void)
{
    return capable && podlink_host_connected() && usb_detect() == USB_INSERTED;
}

bool handoff_pending(void)
{
    return phase >= MATCH && phase <= WAIT_END;
}

bool handoff_hold_source(void)
{
    return handoff_pending() || ((phase == IDLE || phase == IGNORE) && handoff_capable() &&
                                playing() && !cider_active());
}

void handoff_init(void)
{
    mutex_init(&wire);
}

/* Transport state and the output slots belong to wire. PCM only publishes
 * its sample at T. The resident callback never calls back into PodLink. */
static void close_transport(void)
{
    if (transport >= SCHEDULED) pcmbuf_handoff_cancel();
    transport = CLOSED;
    ping[0] = 0;
    outgoing[0] = 0;
}

void handoff_reset(void)
{
    mutex_lock(&wire);
    close_transport();
    capable = false;
    ++link_epoch;
    reply = NONE;
    mutex_unlock(&wire);
}

static bool schedule(char *args);

static void event_record(char event)
{
    snprintf(outgoing, sizeof outgoing, "hf\t1\t%c\t%lu", event, ident);
}

bool handoff_receive(const struct podlink_message *m)
{
    if (m->type != PODLINK_TEXT || m->length < 4 ||
        memcmp(m->data, "HF\t", 3))
        return false;
    mutex_lock(&wire);
    if (m->length == 4 && m->data[3] == '1')
        capable = true;
    else if (transport != CLOSED && m->length > 7 && m->length < 112)
    {
        char text[112], *end;
        memcpy(text, m->data, m->length);
        text[m->length] = 0;
        unsigned long id = strtoul(text + 5, &end, 10);
        if (id == ident && !memcmp(text, "HF\t1\t", 5) && *end == '\t')
        {
            const char *token = ++end;
            char *args = strchr(end, '\t');
            if (args) *args++ = 0;
            else args = text + m->length;
            if (!strcmp(token, "ping") && transport <= ACCEPTED &&
                args[0] >= '0' && args[0] <= '4' && args[1] == '\t')
            {
                size_t n = 0;
                while (args[2 + n] >= '0' && args[2 + n] <= '9') ++n;
                if (n && n < sizeof ping && !args[2 + n])
                {
                    strcpy(ping, args + 2);
                    ping_sequence = args[0];
                    ping_received = current_tick;
                }
            }
            else if (!strcmp(token, "go") && transport == ACCEPTED)
            {
                if (!schedule(args))
                    reply = INVALID_GO;
                else
                {
                    ping[0] = 0;
                    pcmbuf_handoff_fade(start);
                    transport = SCHEDULED;
                    event_record('g');
                }
            }
            else if (!strcmp(token, "at") && transport == SCHEDULED)
            {
                long pos = strtol(args, &end, 10);
                if (args != end && !*end && pos >= 0) projected = pos;
            }
            else if (!strcmp(token, "end") && transport >= SCHEDULED && !*args)
                remote_finished = true;
            else if (!strcmp(token, "fail") && *args && !strchr(args, '\t'))
            {
                close_transport();
                reply = !strcmp(args, "timeout") ? TIMEOUT : FAIL;
            }
            else if (!strcmp(token, "offer") && transport == SYNCING &&
                     strlen(args) < sizeof reply_args)
            {
                reply = OFFER;
                strcpy(reply_args, args);
            }
        }
    }
    mutex_unlock(&wire);
    return true;
}

unsigned int handoff_next(char *record, unsigned int size)
{
    mutex_lock(&wire);
    unsigned long at = transport == SCHEDULED ? pcmbuf_handoff_at() : -1ul;
    if (at != -1ul && projected >= 0 && !outgoing[0])
    {
        error = projected - (long)at;
        if (error < -ALIGN_LIMIT_MS || error > ALIGN_LIMIT_MS)
        {
            close_transport();
            reply = ALIGNMENT;
        }
        else
        {
            transport = VERIFIED;
            event_record('v');
        }
    }

    unsigned int n = strlen(outgoing);
    if (n && n < size)
    {
        memcpy(record, outgoing, n + 1);
        outgoing[0] = 0;
    }
    else if (!n && ping[0] && size >= 80)
    {
        unsigned long elapsed = pcmbuf_handoff_position(-1ul);
        if (elapsed != -1ul)
            n = snprintf(record, size, "hf\t1\tp\t%lu\t%s\t%c\t%lu\t%lu\t%lu",
                         ident, ping, ping_sequence, (unsigned long)(uint32_t)ping_received,
                         (unsigned long)(uint32_t)current_tick, elapsed);
        ping[0] = 0;
    }
    else n = 0;
    mutex_unlock(&wire);
    return n;
}

static void send_event_record(char event)
{
    mutex_lock(&wire);
    close_transport();
    event_record(event);
    mutex_unlock(&wire);
}

static const char *target(void)
{
    return rufin ? "Rufin" : "Cider";
}

static const char *phase_name(void)
{
    switch (phase)
    {
        case MATCH: return "match";
        case QUESTION: return "question";
        case PREPARE: return "prepare";
        case ARMED: return fade_begin ? "fade" : "go";
        case WAIT_END: return "wait-end";
        default: return "done";
    }
}

static const char *timeout_reason(void)
{
    if (phase == MATCH) return "offer";
    if (phase == PREPARE) return "sync-or-schedule";
    if (!fade_begin) return "pcm-start";
    return "verify-or-host-end";
}

static void record_outcome(const char *outcome)
{
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    fdprintf(fd, "v=1 id=%lu target=%s outcome=%s offset_ms=%ld rtt_ms=%lu "
              "T_tick=%lu position_error_ms=%ld startup_ms=%lu uncertainty_ms=%lu "
              "fade_start_tick=%lu fade_end_tick=%lu phase=%s reason=%s\n",
              ident, target(), outcome, offset, rtt, start, error, startup,
              uncertainty, fade_begin, fade_end, phase_name(),
              !strcmp(outcome, "timeout") ? timeout_reason() : outcome);
    close(fd);
}

static void clean_field(char *out, const char *in)
{
    size_t n = in ? strlen(in) : 0;
    if (n >= TRACK_FIELD)
    {
        n = TRACK_FIELD - 1;
        while (n && ((unsigned char)in[n] & 0xc0) == 0x80) --n;
    }
    for (size_t i = 0; i < n; ++i)
        out[i] = (unsigned char)in[i] < 32 ? ' ' : in[i];
    out[n] = 0;
}

void handoff_begin(void)
{
    unsigned int epoch = link_epoch;
    if (!handoff_capable() || !playing() || cider_active() || phase != IDLE)
        return;
    struct mp3entry *track = audio_current_track();
    if (!track || !track->length) return;
    char title[TRACK_FIELD], artist[TRACK_FIELD], album[TRACK_FIELD];
    clean_field(title, track->title ? track->title : track->path);
    clean_field(artist, track->artist);
    clean_field(album, track->album);
    path_crc = podlink_crc32((const unsigned char *)track->path, strlen(track->path));
    mutex_lock(&wire);
    if (!capable || epoch != link_epoch)
    {
        mutex_unlock(&wire);
        return;
    }
    duration = track->length;
    offset = error = rtt = startup = start = uncertainty = fade_begin = fade_end = 0;
    rufin = false;
    sheet = (struct handoff_sheet){ .target = "" };
    ui_epoch = epoch;
    if (!++ident) ++ident;
    transport = SYNCING;
    remote_finished = false;
    projected = -1;
    reply = NONE;
    snprintf(outgoing, sizeof outgoing, "hf\t1\tq\t%lu\t%lu\t%s\t%s\t%s",
             ident, duration, title, artist, album);
    mutex_unlock(&wire);
    deadline = current_tick + MATCH_TICKS;
    phase = MATCH;
}

void handoff_usb_dismissed(void)
{
    if (phase == IDLE && handoff_capable()) phase = IGNORE;
    ui_epoch = link_epoch;
}

bool handoff_prompt_wps(uint32_t epoch)
{
    if (get_current_activity() != ACTIVITY_WPS || !playing() || cider_active())
        return false;
    /* The consent dialog polls while CDC negotiates, keeping input live. */
    if (!usb_confirm_pending(epoch)) return true;
    if (!handoff_capable()) return false;
    usb_confirm_reply(epoch, false); /* keep CDC and withhold mass storage */
    handoff_begin();
    return true;
}

static bool same_track(void)
{
    struct mp3entry *track = audio_current_track();
    return track && track->length == duration &&
           podlink_crc32((const unsigned char *)track->path,
                         strlen(track->path)) == path_crc;
}

static void wait_end(const char *reason)
{
    send_event_record('x');
    pcmbuf_handoff_hold(true);
    record_outcome(reason);
    phase = WAIT_END;
}

static void fail(const char *reason, int message)
{
    wait_end(reason);
    sheet.reason = message;
    failure_until = current_tick + 2 * HZ;
}

static bool switch_remote(const char *outcome, bool stop)
{
    if (!handoff_capable()) return false;
    if (!cider_available()) return false;
    if (stop) audio_stop(); /* saves the current playlist resume point */
    pcmbuf_handoff_hold(false);
    phase = COMPLETE; /* release the source guard before selecting remote */
    cider_remote_source();
    record_outcome(outcome);
    return true;
}

static bool schedule(char *args)
{
    uint32_t field[6];
    for (unsigned int i = 0; i < ARRAYLEN(field); ++i)
    {
        char *end;
        field[i] = strtoul(args, &end, 10);
        if (args == end || *end != (i + 1 == ARRAYLEN(field) ? '\0' : '\t'))
            return false;
        args = end + 1;
    }
    if (!TIME_AFTER(field[0], current_tick + 2) ||
        !TIME_BEFORE(field[0], deadline) ||
        (int32_t)field[3] < -ALIGN_LIMIT_MS || (int32_t)field[3] > ALIGN_LIMIT_MS)
        return false;
    start = field[0];
    offset = (int32_t)field[1];
    rtt = field[2];
    error = (int32_t)field[3];
    startup = field[4];
    uncertainty = field[5];
    return true;
}

static int advance(void)
{
    if (!handoff_capable() || ui_epoch != link_epoch)
    {
        if (handoff_pending())
        {
            mutex_lock(&wire);
            close_transport();
            mutex_unlock(&wire);
            pcmbuf_handoff_hold(false);
            record_outcome(usb_detect() == USB_INSERTED ? "link-lost" : "unplug");
            phase = IDLE;
            sheet.reason = 0;
            return 1;
        }
        if (ui_epoch != link_epoch || usb_detect() != USB_INSERTED) phase = IDLE;
        ui_epoch = link_epoch;
        return 0;
    }
    if (!handoff_pending() || sheet.reason) return 0;

    if (phase == WAIT_END)
    {
        bool ended = pcmbuf_handoff_ended();
        if (ended || !playing())
        {
            bool stop = ended && playing();
            if (switch_remote(ended ? "track-end" : "paused", stop)) return 2;
            if (!handoff_capable()) return 0;
            if (stop) audio_stop();
            pcmbuf_handoff_hold(false);
            phase = COMPLETE;
            record_outcome("remote-unavailable");
            return 1;
        }
        return 0;
    }

    if (!playing() || !same_track())
    {
        send_event_record('x');
        record_outcome("local-change");
        if (playing())
        {
            phase = IDLE;
            handoff_begin();
        }
        else phase = COMPLETE;
        return 1;
    }

    mutex_lock(&wire);
    enum reply received = reply;
    reply = NONE;
    bool ready = transport >= SCHEDULED;
    bool done = remote_finished && transport == VERIFIED;
    if (ready && pcmbuf_handoff_at() != -1ul) fade_begin = start;
    if (received == OFFER && phase == MATCH)
    {
        rufin = !strncmp(reply_args, "Rufin\t", 6);
        if (rufin || !strncmp(reply_args, "Cider\t", 6))
        {
            sheet.target = target();
            if (!strcmp(reply_args + 6, "1")) phase = QUESTION;
            else if (strcmp(reply_args + 6, "0")) received = NONE;
        }
        else received = NONE;
    }
    if (phase == QUESTION && sheet.preparing && transport == SYNCING)
    {
        deadline = current_tick + START_TICKS;
        transport = ACCEPTED;
        phase = PREPARE;
        event_record('a');
    }
    mutex_unlock(&wire);
    if (received >= FAIL)
    {
        fail(received == INVALID_GO ? "invalid-go" : received == ALIGNMENT ? "alignment" :
             received == TIMEOUT ? "timeout" : "host-failed",
             received == TIMEOUT ? LANG_HANDOFF_NO_RESPONSE : LANG_HANDOFF_HOST_FAILED);
        return 1;
    }
    if (received == OFFER && phase == MATCH)
    {
        fail("not-found", LANG_HANDOFF_NOT_FOUND);
        return 1;
    }
    if (phase == PREPARE && ready)
    {
        phase = ARMED;
        deadline = start + FADE_TICKS + HZ;
        sheet.start = start;
        sheet.armed = true;
    }
    if (phase == ARMED && TIME_AFTER(current_tick, start + FADE_TICKS - 1))
    {
        if (fade_begin) fade_end = start + FADE_TICKS;
        if (done && switch_remote("accepted", true))
        {
            dissolve = sheet_open;
            send_event_record('d');
            return 2;
        }
    }
    if (phase != QUESTION && TIME_AFTER(current_tick, deadline))
    {
        fail("timeout", LANG_HANDOFF_NO_RESPONSE);
        return 1;
    }
    return 0;
}

static int sheet_step(int answer)
{
    if (answer == DIALOG_CANCEL && sheet.reason)
    {
        sheet.reason = 0;
        return DIALOG_ABORT;
    }
    if (answer == DIALOG_CANCEL && handoff_pending())
    {
        if (handoff_capable() && ui_epoch == link_epoch)
            wait_end("declined");
        else
            advance();
        return DIALOG_CANCEL;
    }
    if (answer == DIALOG_ACCEPT && !sheet.preparing && !sheet.reason)
    {
        sheet.preparing = true;
    }
    unsigned long proposal = ident;
    if (advance() == 2) return DIALOG_ACCEPT;
    if (proposal != ident) return DIALOG_ABORT;
    if (sheet.reason)
    {
        if (TIME_BEFORE(current_tick, failure_until)) return DIALOG_CONTINUE;
        sheet.reason = 0;
        failure_until = 0;
        return DIALOG_ABORT;
    }
    return phase >= MATCH && phase <= ARMED ? DIALOG_CONTINUE : DIALOG_ABORT;
}

/* 0 = ordinary input, 1 = redraw, 2 = remote WPS, 3 = local WPS. */
int handoff_step(void)
{
    if (sheet_open) return 0;
    int result = advance();
    if ((phase != MATCH && phase != QUESTION) && !sheet.reason) return result;

    char title[TRACK_FIELD];
    struct mp3entry *track = audio_current_track();
    clean_field(title, track ? (track->title ? track->title : track->path) : NULL);
    sheet.title = title;
    sheet_open = true;
    int answer = handoff_sheet_run(&sheet, sheet_step);
    sheet_open = false;
    if (answer == DIALOG_ACCEPT) return 2;
    if (answer == DIALOG_ABORT && phase == WAIT_END) return 3;
    return 1;
}

bool handoff_take_dissolve(void)
{
    bool pending = dissolve;
    dissolve = false;
    return pending;
}
#endif
