/* Standard MIDI File player with original synthesized instruments.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "plugin.h"
#include "usb.h"

#define MIDI_BYTES (512 * 1024)
#define MIDI_TRACKS 32
#define MIDI_EVENTS 65536
#define MIDI_VOICES 12
#define MIDI_SECONDS (30 * 60)

struct midi_track {
    const unsigned char *cursor;
    const unsigned char *end;
    uint32_t time;
    uint32_t tempo;
    uint8_t running;
    uint8_t status;
    uint8_t a;
    uint8_t b;
    bool finished;
};

struct midi_event {
    uint32_t frame;
    uint8_t status;
    uint8_t a;
    uint8_t b;
};

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static bool variable(struct midi_track *track, uint32_t *result)
{
    *result = 0;
    for (int i = 0; i < 4 && track->cursor < track->end; i++)
    {
        uint8_t byte = *track->cursor++;
        *result = (*result << 7) | (byte & 0x7f);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

static bool next_event(struct midi_track *track)
{
    uint32_t delta;
    if (!variable(track, &delta) || track->cursor == track->end ||
        delta > UINT32_MAX - track->time)
        return false;

    track->time += delta;
    track->tempo = 0;
    uint8_t status = *track->cursor;
    if (status & 0x80)
        track->cursor++;
    else
        status = track->running;

    track->status = status;
    if (status >= 0x80 && status < 0xf0)
    {
        unsigned int type = status >> 4;
        size_t length = type == 0xc || type == 0xd ? 1 : 2;
        if ((size_t)(track->end - track->cursor) < length)
            return false;
        track->a = track->cursor[0];
        track->b = length == 2 ? track->cursor[1] : 0;
        if ((track->a | track->b) & 0x80)
            return false;
        track->running = status;
        track->cursor += length;
        return true;
    }

    uint8_t meta = 0;
    if (status == 0xff && track->cursor < track->end)
        meta = *track->cursor++;
    else if (status != 0xf0 && status != 0xf7)
        return false;

    track->running = 0;
    uint32_t length;
    if (!variable(track, &length) ||
        length > (size_t)(track->end - track->cursor))
        return false;
    const unsigned char *data = track->cursor;
    track->cursor += length;

    if (status == 0xff && meta == 0x2f)
    {
        if (length || track->cursor != track->end)
            return false;
        track->finished = true;
    }
    if (status == 0xff && meta == 0x51)
    {
        if (length != 3)
            return false;
        track->tempo = (uint32_t)data[0] << 16 | data[1] << 8 | data[2];
        if (!track->tempo)
            return false;
    }
    return true;
}

static int timeline(const unsigned char *data, size_t length, unsigned int rate,
                    struct midi_event *events, uint32_t *duration)
{
    if (length < 14 || rb->memcmp(data, "MThd", 4) ||
        be32(data + 4) != 6 || data[8] || data[9] > 1 || data[10] ||
        !data[11] || data[11] > MIDI_TRACKS || (data[9] == 0 && data[11] != 1))
        return -1;

    unsigned int count = data[11];
    unsigned int division = data[12] << 8 | data[13];
    if (!division || division & 0x8000)
        return -1;

    struct midi_track tracks[MIDI_TRACKS] = {{0}};
    const unsigned char *end = data + length;
    data += 14;
    for (unsigned int i = 0; i < count; i++)
    {
        if (end - data < 8 || rb->memcmp(data, "MTrk", 4))
            return -1;
        uint32_t bytes = be32(data + 4);
        data += 8;
        if (bytes > (size_t)(end - data))
            return -1;
        tracks[i].cursor = data;
        tracks[i].end = data + bytes;
        if (!next_event(&tracks[i]))
            return -1;
        data += bytes;
    }
    if (data != end)
        return -1;

    uint32_t tick = 0;
    uint32_t tempo = 500000;
    uint64_t denominator = (uint64_t)division * 1000000;
    uint64_t clock = 0;
    uint64_t limit = denominator * rate * MIDI_SECONDS;
    int used = 0;
    for (unsigned int work = 0; work < MIDI_EVENTS; work++)
    {
        if (work % 256 == 0 &&
            rb->default_event_handler(rb->button_get(false)) == SYS_USB_CONNECTED)
            return -1;
        struct midi_track *next = NULL;
        for (unsigned int i = 0; i < count; i++)
        {
            if (tracks[i].status && (!next || tracks[i].time < next->time))
                next = &tracks[i];
        }
        if (!next)
        {
            *duration = clock / denominator;
            return used;
        }

        uint32_t delta = next->time - tick;
        uint64_t step = (uint64_t)rate * tempo;
        if (delta > (limit - clock) / step)
            return -1;
        clock += delta * step;
        tick = next->time;
        if (next->tempo)
            tempo = next->tempo;
        if (next->status < 0xf0)
            events[used++] = (struct midi_event){
                clock / denominator, next->status, next->a, next->b
            };

        if (next->finished)
            next->status = 0;
        else if (!next_event(next))
            return -1;
    }
    return -1;
}

struct midi_voice {
    uint32_t phase;
    uint32_t step;
    uint8_t channel;
    uint8_t note;
    uint8_t shape;
    int level;
    int release;
};

static struct midi_voice voices[MIDI_VOICES];
static uint8_t programs[16];
static uint8_t volume[16];
static uint32_t steps[128];
static uint32_t noise = 1;

static void init_instruments(unsigned int rate)
{
    static const uint32_t octave[] = {
        440000, 466164, 493883, 523251, 554365, 587330,
        622254, 659255, 698456, 739989, 783991, 830609,
    };
    for (int note = 0; note < 128; note++)
    {
        uint64_t frequency = octave[(note + 3) % 12];
        int shift = (note + 3) / 12 - 6;
        frequency = shift < 0 ? frequency >> -shift : frequency << shift;
        steps[note] = (frequency << 32) / ((uint64_t)rate * 1000);
    }
    rb->memset(volume, 100, sizeof(volume));
}

static void channel_event(const struct midi_event *event)
{
    int channel = event->status & 15;
    int kind = event->status >> 4;
    if (kind == 0xc)
        programs[channel] = event->a;
    if (kind == 0xb && event->a == 7)
        volume[channel] = event->b;

    if (kind == 0x9 && event->b)
    {
        struct midi_voice *voice = &voices[0];
        for (size_t i = 0; i < ARRAYLEN(voices); i++)
        {
            struct midi_voice *candidate = &voices[i];
            if (candidate->channel == channel && candidate->note == event->a)
            {
                voice = candidate;
                break;
            }
            if (candidate->level < voice->level)
                voice = candidate;
        }
        *voice = (struct midi_voice){
            .step = steps[event->a],
            .note = event->a,
            .channel = channel,
            .level = event->b * 64,
            .shape = channel == 9 ? 4 : (programs[channel] >> 4) & 3,
            .release = channel == 9 ? 12 : 0,
        };
    }
    else if (kind == 0x8 || kind == 0x9 ||
             (kind == 0xb && (event->a == 120 || event->a == 123)))
    {
        for (size_t i = 0; i < ARRAYLEN(voices); i++)
        {
            struct midi_voice *voice = &voices[i];
            if (voice->channel == channel &&
                (kind == 0xb || voice->note == event->a))
                voice->release = 16;
        }
    }
}

/* Synthesized triangle, pulse, saw and reed waves plus a noise drum. */
static int16_t sample(void)
{
    int sum = 0;
    for (size_t i = 0; i < ARRAYLEN(voices); i++)
    {
        struct midi_voice *voice = &voices[i];
        if (!voice->level)
            continue;
        int phase = voice->phase >> 24;
        int wave;
        switch (voice->shape)
        {
            case 0: wave = 2 * (phase < 128 ? phase : 255 - phase) - 127; break;
            case 1: wave = phase < 128 ? 100 : -100; break;
            case 2: wave = phase - 128; break;
            case 3: wave = phase < 64 ? 110 : -36; break;
            default:
                noise = noise * 1664525u + 1013904223u;
                wave = (int)(noise >> 24) - 128;
                break;
        }
        sum += wave * (voice->level * volume[voice->channel] >> 7);
        voice->phase += voice->step;
        voice->level = MAX(0, voice->level - voice->release);
    }
    return (int16_t)(sum / 512);
}

static enum plugin_status play(const struct midi_event *events, int count,
                               uint32_t duration, unsigned int rate)
{
    static int16_t block[ROCK_STREAM_BLOCK_BYTES / 2];
    uint32_t frame = 0;
    int event = 0;
    bool paused = false;
    init_instruments(rate);
    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "MIDI Player");
    rb->lcd_puts(0, 2, "Play: pause or resume");
    rb->lcd_puts(0, 3, "Menu: return");
    rb->lcd_update();
    duration += rate / 4;
    while (true)
    {
        int pending = rb->rock_stream_pending();
        if (pending < 0 || (!pending && frame >= duration))
            break;
        if (!paused && pending < ROCK_STREAM_QUEUE_SIZE && frame < duration)
        {
            size_t frames = MIN(duration - frame, ARRAYLEN(block) / 2);
            for (size_t i = 0; i < frames; i++, frame++)
            {
                while (event < count && events[event].frame == frame)
                    channel_event(&events[event++]);
                if (frame == duration - rate / 4)
                    for (size_t j = 0; j < ARRAYLEN(voices); j++)
                        voices[j].release = 16;
                block[2 * i] = sample();
                block[2 * i + 1] = block[2 * i];
            }
            if (!rb->rock_stream_submit(block, frames * 4))
                break;
        }
        rb->reset_poweroff_timer();
        int action = rb->get_action(CONTEXT_WPS, 1);
        switch (action)
        {
            case ACTION_WPS_MENU:
            case ACTION_WPS_STOP:
            case ACTION_STD_CANCEL:
                return PLUGIN_OK;
            case ACTION_WPS_PLAY:
                paused = !paused;
                if (!rb->rock_stream_pause(paused))
                    return PLUGIN_OK;
                break;
            case ACTION_WPS_VOLUP:
                rb->adjust_volume(1);
                break;
            case ACTION_WPS_VOLDOWN:
                rb->adjust_volume(-1);
                break;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;
                break;
        }
    }
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    char choice[MAX_PATH];
    const char *path = parameter;
    if (!path)
    {
        if (rb->pick_file(choice, sizeof(choice), "/", "Choose MIDI file",
                          ".mid,.midi", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = choice;
    }

    size_t granted;
    size_t bytes = MIDI_BYTES + MIDI_EVENTS * sizeof(struct midi_event);
    unsigned char *data = rb->plugin_claim_scratch(bytes, bytes, &granted);
    if (!data)
        return PLUGIN_ERROR;

    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return PLUGIN_ERROR;
    off_t size = rb->ffilesize(fd);
    bool read_ok = size > 0 && size <= MIDI_BYTES &&
                   rb->read(fd, data, size) == size;
    if (rb->close(fd) < 0 || !read_ok)
        return PLUGIN_ERROR;

    unsigned int rate = rb->rock_stream_open();
    if (!rate)
    {
        rb->splash(2 * HZ, "Stop playback before opening MIDI Player");
        return PLUGIN_OK;
    }
    struct midi_event *events = (void *)(data + MIDI_BYTES);
    uint32_t duration;
    int count = timeline(data, size, rate, events, &duration);
    enum plugin_status result = PLUGIN_ERROR;
    if (count >= 0)
        result = play(events, count, duration, rate);
    else if (!rb->plugin_usb_taken())
        rb->splash(2 * HZ, "Unsupported or incomplete MIDI file");
    rb->rock_stream_close();
    return result;
}
