/* Firmware-owned plugin services. GNU GPL version 2 or later. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "audio/playback.h"
#include "file.h"
#include "kernel.h"
#include "lang_enum.h"
#include "metadata.h"
#include "playlist/playlist.h"
#include "power.h"
#include "powermgmt.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "storage.h"
#include "timefuncs.h"
#include "usb.h"
#include "plugins/rock_pitch.h"
#include "plugins/rock_resident.h"

#define BATTERY_LOG "/battery_bench.txt"
#define BATTERY_PERIOD (60 * HZ)
#define BENCH_SAMPLES 1000
#define BENCH_END "--Battery bench ended, reason: "

enum bench_message {
    BENCH_START = 1,
    BENCH_STOP,
    BENCH_STATUS,
    BENCH_SHUTDOWN,
    BENCH_DISCARD,
};

struct bench_sample {
    unsigned int seconds;
    short level, minutes, voltage;
    unsigned char flags;
};

enum bench_flags {
    BENCH_CHARGER = 1,
    BENCH_CHARGING = 2,
    BENCH_USB = 4,
};

static struct event_queue bench_queue;
static struct queue_sender_list bench_senders;
static long bench_stack[DEFAULT_STACK_SIZE / sizeof(long) * 2];
static bool bench_ready;
static bool bench_running;
static bool bench_usb;
static bool bench_failed;
static unsigned int bench_count;
static unsigned long bench_start_tick;
static unsigned long bench_next_tick;
static struct bench_sample bench_samples[BENCH_SAMPLES];

static bool append_text(const char *text)
{
    int fd = open(BATTERY_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return false;
    size_t len = strlen(text);
    bool ok = write(fd, text, len) == (ssize_t)len;
    return close(fd) == 0 && ok;
}

static bool flush_samples(void)
{
    if (!bench_count)
        return true;

    int fd = open(BATTERY_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return false;
    bool ok = true;
    for (unsigned int i = 0; i < bench_count; ++i)
    {
        const struct bench_sample *s = &bench_samples[i];
        unsigned int sec = s->seconds;
        char row[96];
        int len = snprintf(row, sizeof(row),
            "%02u:%02u:%02u,  %05u,  %03d%%,  %02d:%02d,  %04d,"
            "  %c,  %c,  %c\n",
            sec / 3600, sec / 60 % 60, sec % 60, sec, s->level,
            s->minutes / 60, s->minutes % 60, s->voltage,
            s->flags & BENCH_CHARGER ? 'A' : '-',
            s->flags & BENCH_CHARGING ? 'C' : '-',
            s->flags & BENCH_USB ? 'U' : '-');
        if (len < 0 || (size_t)len >= sizeof(row) || write(fd, row, len) != len)
        {
            ok = false;
            break;
        }
    }
    bench_count = 0;
    return close(fd) == 0 && ok;
}

static bool bench_storage_active(void)
{
#if CONFIG_PLATFORM & PLATFORM_NATIVE
    return storage_disk_is_active();
#else
    return true;
#endif
}

static void take_sample(void)
{
    if (bench_count < BENCH_SAMPLES)
    {
        struct bench_sample *s = &bench_samples[bench_count++];
        *s = (struct bench_sample){
            .seconds = ((unsigned long)current_tick - bench_start_tick) / HZ,
            .level = battery_level(),
            .minutes = battery_time(),
            .voltage = battery_voltage(),
            .flags = (charger_inserted() ? BENCH_CHARGER : 0)
#if CONFIG_CHARGING >= CHARGING_MONITOR
                   | (charging_state() ? BENCH_CHARGING : 0)
#endif
                   | (usb_inserted() ? BENCH_USB : 0),
        };
    }

    /* Keep the disk asleep during a measurement. */
    if (battery_level_safe() &&
        (bench_storage_active() || bench_count == BENCH_SAMPLES) &&
        !flush_samples())
    {
        bench_failed = true;
        bench_running = false;
    }
    bench_next_tick = (unsigned long)current_tick + BATTERY_PERIOD;
}

static bool bench_start(void)
{
    if (bench_usb || usb_host_wants_storage() || !battery_level_safe())
        return false;
    if (bench_running)
        return true;

    if (!append_text("\n# Battery Benchmark " MODEL_NAME "\n"
                     "# Time, Seconds, Level, Time Left, Voltage[mV],"
                     " A, C, U\n"))
        return false;
    bench_running = true;
    bench_failed = false;
    bench_count = 0;
    bench_start_tick = current_tick;
    take_sample();
    return bench_running;
}

static bool bench_stop(bool disk_safe, const char *footer)
{
    bool ok = !bench_failed;

    if (bench_running && disk_safe)
        ok = !bench_usb && !usb_host_wants_storage() && battery_level_safe() &&
             flush_samples() && append_text(footer);

    bench_running = false;
    bench_failed = false;
    bench_count = 0;
    return ok;
}

static void bench_thread(void)
{
    while (true)
    {
        struct queue_event ev;
        long timeout = TIMEOUT_BLOCK;
        if (bench_running && !bench_usb)
        {
            long remaining = bench_next_tick - (unsigned long)current_tick;
            timeout = MAX(0, remaining);
        }
        queue_wait_w_tmo(&bench_queue, &ev, timeout);
        switch (ev.id)
        {
        case SYS_TIMEOUT:
            take_sample();
            break;
        case SYS_USB_CONNECTED:
            /* A sample holds its file only for the duration of one append. */
            bench_usb = true;
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            break;
        case SYS_USB_DISCONNECTED:
            bench_usb = false;
            break;
        case BENCH_START:
            queue_reply(&bench_queue, bench_start());
            break;
        case BENCH_STOP:
            queue_reply(&bench_queue,
                        bench_stop(true, BENCH_END "plugin exit--\n"));
            break;
        case BENCH_SHUTDOWN:
            queue_reply(&bench_queue,
                        bench_stop(true, BENCH_END "power off--\n"));
            break;
        case BENCH_DISCARD:
            queue_reply(&bench_queue, bench_stop(false, NULL));
            break;
        case BENCH_STATUS:
            queue_reply(&bench_queue, bench_running);
            break;
        }
    }
}

bool rock_battery_bench_running(void)
{
    return bench_ready && queue_send(&bench_queue, BENCH_STATUS, 0);
}

bool rock_battery_bench_start(void)
{
    if (!bench_ready)
    {
        queue_init(&bench_queue, true);
        unsigned int id = create_thread(bench_thread, bench_stack,
                         sizeof(bench_stack), 0, "Battery Benchmark"
                         IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
        if (id == 0 || id == (unsigned int)-1)
        {
            queue_delete(&bench_queue);
            return false;
        }
        queue_enable_queue_send(&bench_queue, &bench_senders, id);
        bench_ready = true;
    }
    return queue_send(&bench_queue, BENCH_START, 0);
}

bool rock_battery_bench_stop(void)
{
    return !bench_ready || queue_send(&bench_queue, BENCH_STOP, 0);
}

static struct rock_announce_status announcement = {
    .interval = 10,
    .format = "D1Dt ",
};
static volatile bool announce_on_track;
static unsigned int announce_index;
static unsigned long announce_deadline;

void rock_announce_status_get(struct rock_announce_status *options)
{
    *options = announcement;
}

bool rock_announce_status_set(const struct rock_announce_status *options)
{
    if (options->interval < 1 || options->interval > 360 ||
        options->grouping < 0 || options->grouping > 7 ||
        options->format[ROCK_ANNOUNCE_FORMAT_MAX] != '\0')
        return false;

    announcement = *options;
    announce_index = 0;
    announce_deadline = current_tick;
    announce_on_track = options->track_change;
    return true;
}

bool rock_announce_status_on_track_change(void)
{
    return announce_on_track;
}

static void say_track(int token, const struct mp3entry *track)
{
    if (!track)
        return;
    switch (token)
    {
    case 'E': talk_value(track->elapsed / 1000, UNIT_TIME, true); break;
    case 'L': talk_value(track->length / 1000, UNIT_TIME, true); break;
    case 'R':
        talk_value(track->elapsed < track->length ?
                   (track->length - track->elapsed) / 1000 : 0,
                   UNIT_TIME, true);
        break;
    case 'T': talk_name(track->title ? track->title : track->path, true); break;
    case 'A':
        if (track->artist || track->albumartist)
            talk_name(track->artist ? track->artist : track->albumartist, true);
        break;
    case '1': talk_id(LANG_ELAPSED, true); break;
    case '2': talk_id(LANG_REMAIN, true); break;
    case '3': talk_id(LANG_OF, true); break;
    }
}

static void say_playlist(int token)
{
    int total = playlist_amount();
    if (total <= 0)
        return;
    switch (token)
    {
    case 'C': talk_number(playlist_get_display_index(), true); break;
    case 'N': talk_number(total, true); break;
    case 'R': talk_number(total - playlist_get_display_index(), true); break;
    case '1': talk_id(LANG_TRACK, true); break;
    case '2': talk_id(LANG_OF, true); break;
    }
}

static void say_power(int category, int token)
{
    if (category == 'B')
    {
        if (token == '1')
            talk_id(LANG_BATTERY_TIME, true);
        else if (token == 'P')
            talk_value(battery_level(), UNIT_PERCENT, true);
        else if (token == 'M')
            talk_value(battery_time() * 60, UNIT_TIME, true);
        return;
    }
    if (token == '1')
        talk_id(LANG_RUNNING_TIME, true);
    else if (token == 'T')
        talk_value(global_status.runtime, UNIT_TIME, true);
    else
    {
        int remaining = get_sleep_timer();
        if (remaining > 0)
        {
            if (token == '2')
                talk_id(LANG_SLEEP_TIMER, true);
            else if (token == '3')
                talk_id(LANG_REMAIN, true);
            else if (token == 'S')
                talk_value(remaining, UNIT_TIME, true);
        }
    }
}

static void say_date_time(int token)
{
    if (token == '1')
        talk_id(LANG_TIME, true);
    else if (token == '2')
        talk_id(LANG_DATE, true);
#if CONFIG_RTC
    else if (token == 'T')
        talk_time(get_time(), true);
    else if (token == 'D')
        talk_date(get_time(), true);
#endif
}

bool rock_announce_status_speak(void)
{
    if (!global_settings.talk_menu || rock_pitch_suppresses_tones() ||
        (audio_status() & (AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD)))
        return false;

    const char *format = announcement.format;
    if (!format[0])
        format = "D1Dt ";
    size_t length = strlen(format);
    if (announce_index >= length)
        announce_index = 0;
    const unsigned char *s = (const unsigned char *)format + announce_index;

    struct mp3entry snapshot;
    const struct mp3entry *track =
        audio_current_track_snapshot(&snapshot) ? &snapshot : NULL;
    unsigned int groups = 0;
    for (; *s; ++s)
    {
        int category = toupper(*s);
        if (category == ';' && announcement.grouping > 0 &&
            ++groups == (unsigned int)announcement.grouping)
        {
            ++s;
            break;
        }
        if (category == ' ')
        {
            talk_id(VOICE_PAUSE, true);
            continue;
        }
        if (!s[1] || !strchr("DTPBR", category))
            continue;

        int token = toupper(*++s);
        switch (category)
        {
        case 'D': say_date_time(token); break;
        case 'T': say_track(token, track); break;
        case 'P': say_playlist(token); break;
        case 'B':
        case 'R': say_power(category, token); break;
        }
    }
    announce_index = s - (const unsigned char *)format;
    return true;
}

void rock_announce_status_track_change(void)
{
    if (!announce_on_track || TIME_BEFORE(current_tick, announce_deadline))
        return;
    announce_deadline = (unsigned long)current_tick +
                        announcement.interval * HZ;
    rock_announce_status_speak();
}

void rock_resident_shutdown(bool disk_safe)
{
    announce_on_track = false;
    announcement.track_change = false;
    if (bench_ready)
        queue_send(&bench_queue, disk_safe ? BENCH_SHUTDOWN : BENCH_DISCARD, 0);
}
