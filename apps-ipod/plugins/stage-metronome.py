#!/usr/bin/env python3
"""Stage Metronome for the foreground tick and click leases."""
from pathlib import Path
import sys


def replace(old, new, count=1):
    global source
    if source.count(old) != count:
        raise SystemExit(f"{source_path}: expected {count} matches for {old!r}")
    source = source.replace(old, new)


source_path = Path(sys.argv[1])
source = source_path.read_text()
replace('#include "fixedpoint.h"', '''#include "fixedpoint.h"

static bool metronome_space(char c)
{
    return c == ' ' || (c >= '\\t' && c <= '\\r');
}''')
replace('isspace(*p)', 'metronome_space(*p)')
replace('isspace(*line)', 'metronome_space(*line)')
replace('isdigit(*p)', "(*p >= '0' && *p <= '9')", count=3)
replace('''#if defined(SIMULATOR)
/* Simulator really wants 1024. Not 1000, not 512, only 1024.
   Otherwise it is strangely slow. */
static const unsigned int timerfreq_div = 1024;
#else
static const unsigned int timerfreq_div = 500; /* 2 ms resolution */
#endif''', '''static const unsigned int timerfreq_div = ROCK_CLICK_TICK_HZ;''')
replace('static bool sound_paused = true;', '''static bool sound_paused = true;
static bool timer_failed;
static int click_volume;''')
replace('rb->global_status->volume', 'click_volume', count=6)
replace('rb->sound_set(SOUND_VOLUME, vol);', 'rb->click_volume(vol);')
replace('rb->sound_min(SOUND_VOLUME)', 'rb->click_min_volume()', count=2)
replace('rb->sound_max(SOUND_VOLUME)', 'rb->click_max_volume()', count=2)
replace('#define METRONOME_PAUSE         PLA_CANCEL',
        '#define METRONOME_PAUSE         PLA_DOWN')
replace('''/* Really necessary? Cannot just play mono?
   Also: This is wasted memory! */
static short tick_buf[sizeof(tick_sound)*2];
static short tock_buf[sizeof(tock_sound)*2];

/* Convert the mono samples to interleaved stereo */
static void prepare_buffers(void)
{
    size_t i;
    for(i = 0;i < sizeof(tick_sound)/sizeof(short);i++)
      tick_buf[i*2] = tick_buf[i*2+1] = tick_sound[i];
    for(i = 0;i < sizeof(tock_sound)/sizeof(short);i++)
      tock_buf[i*2] = tock_buf[i*2+1] = tock_sound[i];
}

static void play_tick(void)
{
    rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, NULL, tick_buf, sizeof(tick_buf));
}

static void play_tock(void)
{
    rb->mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, NULL, tock_buf, sizeof(tock_buf));
}''', '''/* Fit the clicks to the native sink rate and leave its route unchanged. */
static int32_t tick_buf[ARRAYLEN(tick_sound) * SAMPR_192 / SAMPR_44];
static int32_t tock_buf[ARRAYLEN(tock_sound) * SAMPR_192 / SAMPR_44];
static size_t tick_size, tock_size;

static size_t prepare_click(int32_t *out, const short *mono, size_t count,
                            unsigned int rate)
{
    size_t frames = count * rate / SAMPR_44;
    for (size_t i = 0; i < frames; ++i)
    {
        uint32_t sample = (uint16_t)mono[i * SAMPR_44 / rate];
        out[i] = sample | (sample << 16);
    }
    return frames * sizeof(*out);
}

static void play_tick(void)
{
    rb->click_play(tick_buf, tick_size);
}

static void play_tock(void)
{
    rb->click_play(tock_buf, tock_size);
}''')
replace('''        if(sound_paused) display->puts(0,2,"start: hold select");''',
        '''        if(timer_failed) display->puts(0,2,"Timer unavailable. Retry Select.");
        else if(sound_paused) display->puts(0,2,"start: hold select");''')
replace('display->puts(0,2,"stop : cancel");',
        'display->puts(0,2,"pause: Play/Pause");')
replace('''                case METRONOME_PLAY:
                    if(sound_paused) metronome_unpause();''',
        '''                case METRONOME_PLAY:
                    if(sound_paused && last_button != METRONOME_PLAY)
                        metronome_unpause();''')
replace('''    if(beating)
    {
        /* Finish the current beat. */''', '''    rb->ticks_stop();
    sound_trigger = false;
    if(beating)
    {
        /* Finish the current beat. */''')
replace('''    trigger_display(0);
    rb->timer_unregister();''', '''    trigger_display(0);''')
replace('''static void metronome_unpause(void)
{
    sound_paused = false;
    minitick = period; /* Start playing immediately (or after a millisecond). */
    /* Conserve power: Only start timer when actually playing. */
    rb->timer_register( 1, NULL, TIMER_FREQ/timerfreq_div
                      , timer_callback IF_COP(, CPU) );
}''', '''static bool metronome_unpause(void)
{
    if (!rb->ticks_start())
    {
        timer_failed = true;
        trigger_display(0);
        if (track_mode)
            rb->splash(HZ, "Timer unavailable. Retry Select.");
        return false;
    }
    timer_failed = false;
    sound_paused = false;
    minitick = period; /* Start on the next timer tick. */
    return true;
}''')
replace('''    metronome_pause();
    rb->pcmbuf_fade(false, false); /* Mute channel */
    rb->mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
    tweak_volume(0);
    rb->led(0);
    rb->mixer_set_frequency(HW_SAMPR_DEFAULT);''', '''    rb->ticks_stop();''')
replace('''    rb->audio_stop();

    prepare_buffers();
#if INPUT_SRC_CAPS != 0
    /* Select playback */
    rb->audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    rb->audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif
    rb->mixer_set_frequency(SAMPR_44);
    rb->pcmbuf_fade(false, true); /* Be sure channel is audible */''', '''    unsigned int rate = rb->click_begin();
    if (!rate || rate > SAMPR_192)
    {
        rb->splash(HZ, "Click playback unavailable");
        return PLUGIN_ERROR;
    }
    click_volume = rb->global_status->volume;
    tick_size = prepare_click(tick_buf, tick_sound, ARRAYLEN(tick_sound), rate);
    tock_size = prepare_click(tock_buf, tock_sound, ARRAYLEN(tock_sound), rate);''')
replace('''        if(sound_trigger)
        {
            sound_trigger = false;
            play_ticktock(); /* Draws display before playback. */
        }''', '''        /* Overdue ticks update timing; the main loop submits at most one click. */
        for (unsigned int ticks = rb->ticks_take(); ticks && !sound_paused; --ticks)
            timer_callback();
        if(sound_trigger)
        {
            sound_trigger = false;
            play_ticktock(); /* Draws display before playback. */
            if (sound_paused)
                rb->ticks_stop();
        }''')
replace('''                        if(sound_paused) metronome_unpause();
                        tap();''', '''                        if(!sound_paused || metronome_unpause())
                            tap();''')
replace('''                exit_on_usb(button);''', '''                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;''')
replace('''        if(button)
            last_button''', '''        if (!rb->click_active())
            return PLUGIN_OK;

        if(button)
            last_button''')
# The native raw screen expects widths and endpoints in range.
replace('display->hline(0, display->lcdwidth, 12);',
        'display->hline(0, display->lcdwidth - 1, 12);')
replace('''                                 , display->lcdwidth,   rectangle_height );''',
        '''                                 , display->lcdwidth/2, rectangle_height );''')
replace('''                                 , display->lcdwidth,                      rectangle_height );''',
        '''                                 , display->lcdwidth/2,                    rectangle_height );''')

Path(sys.argv[2]).write_text(source)
