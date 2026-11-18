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
#define START_TICKS (5 * HZ)
#define LOG_PATH "/.rockbox/handoff-log.txt"

enum phase { IDLE, MATCH, QUESTION, PREPARE, ARMED, WAIT_END, COMPLETE, IGNORE };
enum reply { NONE, OFFER, PING, GO, AT, END, FAIL };

static struct mutex wire;
static char outgoing[3 * TRACK_FIELD + 48];
static struct {
    enum reply kind;
    long received;
    char args[112];
} incoming;
static volatile bool capable;
static volatile unsigned int link_epoch;
static unsigned int ui_epoch;
static enum phase phase; /* UI thread only */
static unsigned long ident, deadline, start, fade_begin, fade_end;
static unsigned long rtt, startup, uncertainty, duration;
static long offset, error;
static bool rufin, remote_finished, verified, sheet_open, dissolve;
static struct handoff_sheet sheet;
static long failure_until;
static unsigned int pings;
static unsigned long position_at_start;
static long host_position_at_start;
static uint32_t path_crc;
static char title[TRACK_FIELD];

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

void handoff_reset(void)
{
    mutex_lock(&wire);
    capable = false;
    ++link_epoch;
    incoming.kind = NONE;
    outgoing[0] = 0;
    mutex_unlock(&wire);
}

bool handoff_receive(const struct podlink_message *m)
{
    if (m->type != PODLINK_TEXT || m->length < 4 ||
        memcmp(m->data, "HF\t", 3))
        return false;
    mutex_lock(&wire);
    if (m->length == 4 && m->data[3] == '1')
        capable = true;
    else if (capable && m->length < sizeof incoming.args && m->length > 7)
    {
        char text[sizeof incoming.args], *end;
        memcpy(text, m->data, m->length);
        text[m->length] = 0;
        unsigned long id = strtoul(text + 5, &end, 10);
        if (id == ident && !memcmp(text, "HF\t1\t", 5) && *end == '\t')
        {
            const char *token = ++end;
            char *args = strchr(end, '\t');
            if (args) *args++ = 0;
            enum reply kind = NONE;
            if (!strcmp(token, "offer")) kind = OFFER;
            if (!strcmp(token, "ping")) kind = PING;
            if (!strcmp(token, "go")) kind = GO;
            if (!strcmp(token, "at")) kind = AT;
            if (!strcmp(token, "end")) kind = END;
            if (!strcmp(token, "fail")) kind = FAIL;
            if (kind != NONE)
            {
                incoming.kind = kind;
                incoming.received = current_tick;
                strcpy(incoming.args, args ? args : "");
            }
        }
    }
    mutex_unlock(&wire);
    return true;
}

unsigned int handoff_next(char *record, unsigned int size)
{
    mutex_lock(&wire);
    unsigned int n = strlen(outgoing);
    if (n < size && n)
    {
        memcpy(record, outgoing, n + 1);
        outgoing[0] = 0;
    }
    else n = 0;
    mutex_unlock(&wire);
    return n;
}

static void send_event_record(char event)
{
    mutex_lock(&wire);
    snprintf(outgoing, sizeof outgoing, "hf\t1\t%c\t%lu", event, ident);
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
        case PREPARE: return pings == 5 ? "prepare" : "sync";
        case ARMED: return verified ? "fade" : "go";
        case WAIT_END: return "wait-end";
        default: return "done";
    }
}

static const char *timeout_reason(void)
{
    if (phase == MATCH) return "offer";
    if (phase == PREPARE) return pings == 5 ? "schedule" : "ping";
    if (!fade_begin) return "pcm-start";
    if (!verified) return "position";
    return remote_finished ? "remote-source" : "host-end";
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
    if (!handoff_capable() || !playing() || cider_active() || phase != IDLE)
        return;
    struct mp3entry *track = audio_current_track();
    if (!track || !track->length) return;
    char artist[TRACK_FIELD], album[TRACK_FIELD];
    clean_field(title, track->title ? track->title : track->path);
    clean_field(artist, track->artist);
    clean_field(album, track->album);
    path_crc = podlink_crc32((const unsigned char *)track->path, strlen(track->path));
    duration = track->length;
    offset = error = rtt = startup = start = uncertainty = fade_begin = fade_end = 0;
    remote_finished = rufin = verified = false;
    pings = 0;
    sheet = (struct handoff_sheet){ .title = title };
    position_at_start = 0;
    host_position_at_start = -1;
    mutex_lock(&wire);
    if (!++ident) ++ident;
    incoming.kind = NONE;
    snprintf(outgoing, sizeof outgoing, "hf\t1\tq\t%lu\t%lu\t%s\t%s\t%s",
             ident, duration, title, artist, album);
    mutex_unlock(&wire);
    deadline = current_tick + START_TICKS;
    ui_epoch = link_epoch;
    phase = MATCH;
    sheet.target = target();
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
    /* CDC was enabled before consent. Give the daemon time to negotiate.
     * Old peers still receive the ordinary disk consent prompt. */
    long until = current_tick + HZ;
    while (!handoff_capable() && TIME_BEFORE(current_tick, until) &&
           usb_confirm_pending(epoch) && playing())
        sleep(1);
    if (!usb_confirm_pending(epoch)) return true;
    if (!handoff_capable() || !playing()) return false;
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
    pcmbuf_handoff_cancel();
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
    pcmbuf_handoff_cancel();
    pcmbuf_handoff_hold(false);
    phase = COMPLETE; /* release the source guard before selecting remote */
    cider_remote_source();
    record_outcome(outcome);
    return true;
}

static enum reply take_reply(char *args, long *received)
{
    mutex_lock(&wire);
    enum reply reply = incoming.kind;
    if (reply != NONE)
    {
        strcpy(args, incoming.args);
        *received = incoming.received;
        incoming.kind = NONE;
    }
    mutex_unlock(&wire);
    return reply;
}

static bool decimal(const char *text)
{
    const char *start = text;
    while (*text >= '0' && *text <= '9') ++text;
    return text != start && !*text;
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
    start = (unsigned long)field[0];
    offset = (int32_t)field[1];
    rtt = field[2];
    error = (int32_t)field[3];
    startup = field[4];
    uncertainty = field[5];
    return error >= -50 && error <= 50 &&
           TIME_AFTER(start, current_tick + 2) && TIME_BEFORE(start, deadline);
}

static int advance(void)
{
    if (!handoff_capable() || ui_epoch != link_epoch)
    {
        if (handoff_pending())
        {
            pcmbuf_handoff_cancel();
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
        pcmbuf_handoff_cancel();
        record_outcome("local-change");
        if (playing())
        {
            phase = IDLE;
            handoff_begin();
        }
        else phase = COMPLETE;
        return 1;
    }

    char args[sizeof incoming.args];
    long received = 0;
    enum reply reply = take_reply(args, &received);
    if (reply == FAIL && phase != QUESTION && args[0] && !strchr(args, '\t'))
    {
        if (!strcmp(args, "timeout"))
            fail("timeout", LANG_HANDOFF_NO_RESPONSE);
        else
            fail("host-failed", LANG_HANDOFF_HOST_FAILED);
        return 1;
    }
    if (phase == MATCH && reply == OFFER)
    {
        rufin = !strncmp(args, "Rufin\t", 6);
        if (!rufin && strncmp(args, "Cider\t", 6)) return 0;
        sheet.target = target();
        if (!strcmp(args + 6, "0"))
            fail("not-found", LANG_HANDOFF_NOT_FOUND);
        else if (!strcmp(args + 6, "1"))
            phase = QUESTION;
        return 1;
    }
    if (phase == PREPARE && reply == PING)
    {
        if (args[0] >= '0' && args[0] <= '4' && args[1] == '\t' &&
            decimal(args + 2))
        {
            pings = args[0] - '0' + 1;
            unsigned long elapsed = pcmbuf_handoff_position(audio_current_track()->elapsed);
            mutex_lock(&wire);
            snprintf(outgoing, sizeof outgoing, "hf\t1\tp\t%lu\t%s\t%c\t%lu\t%lu\t%lu",
                     ident, args + 2, args[0], (unsigned long)(uint32_t)received,
                     (unsigned long)(uint32_t)current_tick, elapsed);
            mutex_unlock(&wire);
        }
    }
    if (phase == PREPARE && reply == GO)
    {
        if (!schedule(args))
        {
            fail("invalid-go", LANG_HANDOFF_HOST_FAILED);
            return 1;
        }
        pcmbuf_handoff_fade(start);
        phase = ARMED;
        deadline = start + FADE_TICKS + HZ;
        sheet.start = start;
        sheet.armed = true;
        send_event_record('g');
    }
    if (phase == ARMED)
    {
        if (reply == END && !args[0]) remote_finished = true;
        if (reply == AT)
        {
            char *end;
            long pos = strtol(args, &end, 10);
            if (!*end && args != end && pos >= 0) host_position_at_start = pos;
        }
        fade_begin = pcmbuf_handoff_fade_begin();
        if (fade_begin && !position_at_start)
        {
            unsigned long elapsed = pcmbuf_handoff_position(audio_current_track()->elapsed);
            unsigned long late = (current_tick - start) * (1000 / HZ);
            position_at_start = elapsed > late ? elapsed - late : 1;
        }
        if (!verified && position_at_start && host_position_at_start >= 0)
        {
            error = host_position_at_start - (long)position_at_start;
            if (error < -50 || error > 50)
            {
                fail("alignment", LANG_HANDOFF_HOST_FAILED);
                return 1;
            }
            verified = true;
            send_event_record('v');
        }
        if (fade_begin && TIME_AFTER(current_tick, start + FADE_TICKS - 1))
        {
            fade_end = start + FADE_TICKS;
            if (remote_finished && verified && switch_remote("accepted", true))
            {
                dissolve = sheet_open;
                send_event_record('d');
                return 2;
            }
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
    if (advance() == 2) return DIALOG_ACCEPT;
    if (sheet.reason)
    {
        if (TIME_BEFORE(current_tick, failure_until)) return DIALOG_CONTINUE;
        sheet.reason = 0;
        failure_until = 0;
        return DIALOG_ABORT;
    }
    if (phase == QUESTION)
    {
        if (answer == DIALOG_CANCEL)
        {
            wait_end("declined");
            return DIALOG_CANCEL;
        }
        if (answer == DIALOG_ACCEPT)
        {
            phase = PREPARE;
            sheet.preparing = true;
            sheet.accepted = current_tick;
            deadline = current_tick + START_TICKS;
            send_event_record('a');
        }
    }
    return phase == QUESTION || phase == PREPARE || phase == ARMED ?
        DIALOG_CONTINUE : DIALOG_ABORT;
}

/* 0 = ordinary input, 1 = redraw, 2 = remote WPS, 3 = local WPS. */
int handoff_step(void)
{
    if (sheet_open) return 0;
    int result = advance();
    if (phase != QUESTION && !sheet.reason) return result;

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
