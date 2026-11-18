/* PodBox continuity handoff. GNU General Public License, version 2+. */
#include "handoff.h"
#ifdef HAVE_USB_PODLINK
#include "audio.h"
#include "audio/pcmbuf.h"
#include "backlight.h"
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
#include "widgets/splash.h"
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
static bool rufin, remote_finished, verified, dialog_open;
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

static void record_outcome(const char *outcome)
{
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    fdprintf(fd, "v=1 id=%lu target=%s outcome=%s offset_ms=%ld rtt_ms=%lu "
              "T_tick=%lu position_error_ms=%ld startup_ms=%lu uncertainty_ms=%lu "
              "fade_start_tick=%lu fade_end_tick=%lu\n",
              ident, target(), outcome, offset, rtt, start, error, startup,
              uncertainty, fade_begin, fade_end);
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

struct question {
    char body[148];
    bool leave;
};

static void measure_question(struct dialog *d, struct screen *s,
                             struct viewport *box, void *data)
{
    (void)data;
    struct dialog_insets in;
    dialog_get_insets(&d->style, &in);
    box->width = s->lcdwidth - 24;
    box->height = 4 * s->getcharheight() + in.top + in.bottom + 8;
    box->x = 12;
    box->y = (s->lcdheight - box->height) / 2;
}

static void draw_question(struct dialog *d, struct screen *s,
                          struct viewport *content, void *data)
{
    struct question *q = data;
    s->set_viewport(content);
    content->flags |= VP_FLAG_ALIGN_CENTER;
    int h = s->getcharheight();
    s->putsxy(0, 0, str(LANG_HANDOFF_TITLE));
    /* The viewport clips long titles at its edges. */
    s->putsxy(0, h, q->body);
    int width = (content->width - 4) / 2;
    int y = 2 * h + 4;
    dialog_draw_button(s, &d->style, 0, y, width, 2 * h,
                       str(LANG_HANDOFF_ACCEPT), !q->leave);
    dialog_draw_button(s, &d->style, width + 4, y, width, 2 * h,
                       str(LANG_HANDOFF_LEAVE), q->leave);
}

static int question_action(struct dialog *d, int action, void *data)
{
    struct question *q = data;
    if (!handoff_capable() || !playing() || !same_track()) return DIALOG_ABORT;
    if (!d->backlight_on) return DIALOG_CONTINUE;
    switch (action)
    {
        case ACTION_STD_PREV: q->leave = false; break;
        case ACTION_STD_NEXT: q->leave = true; break;
        case ACTION_STD_OK: return q->leave ? DIALOG_CANCEL : DIALOG_ACCEPT;
        case ACTION_STD_MENU:
        case ACTION_STD_CANCEL: return DIALOG_CANCEL;
        default:
            if (IS_SYSEVENT(action) && action != SYS_USB_CONFIRM)
                default_event_handler(action);
            break;
    }
    return DIALOG_CONTINUE;
}

static int ask(void)
{
    static const struct dialog_callbacks callbacks = {
        .measure = measure_question,
        .draw = draw_question,
        .on_action = question_action,
    };
    struct question q = { .leave = false };
    snprintf(q.body, sizeof q.body, str(LANG_HANDOFF_BODY), target(), title);
    struct dialog d;
    dialog_init(&d, CONTEXT_STD, NULL, NULL, &callbacks, &q);
    backlight_on();
    dialog_open = true;
    int result = dialog_run(&d, HZ / 10);
    dialog_open = false;
    action_wait_for_release();
    button_queue_post(BUTTON_REDRAW, 0);
    return result;
}

static void wait_end(const char *reason)
{
    send_event_record('x');
    pcmbuf_handoff_cancel();
    pcmbuf_handoff_hold(true);
    phase = WAIT_END;
    record_outcome(reason);
}

static void fail(const char *reason, int message)
{
    wait_end(reason);
    splashf(HZ, "%s", str(message));
}

static bool switch_remote(const char *outcome, bool stop)
{
    if (!handoff_capable()) return false;
    if (!cider_available()) return false;
    if (stop) audio_stop(); /* saves the current playlist resume point */
    pcmbuf_handoff_cancel();
    pcmbuf_handoff_hold(false);
    phase = COMPLETE;
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

static bool schedule(char *args)
{
    /* Strtol also handles the signed offset and signed position error. */
    long field[6];
    for (unsigned int i = 0; i < ARRAYLEN(field); ++i)
    {
        char *end;
        field[i] = (long)strtoul(args, &end, 10);
        if (args == end || *end != (i + 1 == ARRAYLEN(field) ? '\0' : '\t'))
            return false;
        args = end + 1;
    }
    start = (unsigned long)field[0];
    offset = field[1];
    rtt = field[2];
    error = field[3];
    startup = field[4];
    uncertainty = field[5];
    return error >= -50 && error <= 50 &&
           TIME_AFTER(start, current_tick + 2) && TIME_BEFORE(start, deadline);
}

/* 0 = ordinary input, 1 = redraw this screen, 2 = switch to remote WPS. */
int handoff_step(void)
{
    if (dialog_open) return 0;
    if (!handoff_capable() || ui_epoch != link_epoch)
    {
        if (handoff_pending())
        {
            pcmbuf_handoff_cancel();
            pcmbuf_handoff_hold(false);
            record_outcome(usb_detect() == USB_INSERTED ? "link-lost" : "unplug");
            phase = IDLE;
            return 1;
        }
        if (ui_epoch != link_epoch || usb_detect() != USB_INSERTED) phase = IDLE;
        ui_epoch = link_epoch;
        return 0;
    }
    if (!handoff_pending()) return 0;

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
    long received;
    enum reply reply = take_reply(args, &received);
    if (reply == FAIL)
    {
        fail("host-failed", LANG_HANDOFF_FAILED);
        return 1;
    }
    if (phase == MATCH && reply == OFFER)
    {
        rufin = !strncmp(args, "Rufin\t", 6);
        const char *match = strchr(args, '\t');
        if (!match || strcmp(match + 1, "1"))
        {
            fail("not-found", LANG_HANDOFF_NOT_FOUND);
            return 1;
        }
        phase = QUESTION;
        int answer = ask();
        if (!handoff_capable() || answer == DIALOG_ABORT) return handoff_step();
        if (answer != DIALOG_ACCEPT)
            wait_end("declined");
        else
        {
            phase = PREPARE;
            deadline = current_tick + START_TICKS;
            send_event_record('a');
        }
        return 1;
    }
    if (phase == PREPARE && reply == PING)
    {
        char *separator = strchr(args, '\t');
        if (separator)
        {
            *separator++ = 0;
            unsigned long elapsed = pcmbuf_handoff_position(audio_current_track()->elapsed);
            mutex_lock(&wire);
            snprintf(outgoing, sizeof outgoing, "hf\t1\tp\t%lu\t%s\t%s\t%lu\t%lu\t%lu",
                     ident, separator, args, (unsigned long)received,
                     (unsigned long)current_tick, elapsed);
            mutex_unlock(&wire);
        }
    }
    if (phase == PREPARE && reply == GO && schedule(args))
    {
        pcmbuf_handoff_fade(start);
        phase = ARMED;
        deadline = start + FADE_TICKS + HZ;
        send_event_record('g');
    }
    if (phase == ARMED)
    {
        if (reply == END) remote_finished = true;
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
                fail("alignment", LANG_HANDOFF_FAILED);
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
                send_event_record('d');
                return 2;
            }
        }
    }
    if (TIME_AFTER(current_tick, deadline))
    {
        fail("timeout", LANG_HANDOFF_FAILED);
        return 1;
    }
    return 0;
}
#endif
