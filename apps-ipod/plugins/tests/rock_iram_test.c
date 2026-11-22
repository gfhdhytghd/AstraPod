/* Build rock_iram.c with the host doubles in rock_iram_test.sh. GPL v2+. */
#include <assert.h>
#include "plugins/rock_iram.h"
#include "plugins/rock_iram_layout.h"
#include "pcm.h"

static bool usb_connected, usb_reserved, track_busy, playing, cached;
static bool usb_on_stop, refuse_reservation;
static int irq_depth;
static unsigned int stops, unloads;
static unsigned int caller = 1;

unsigned int thread_self(void) { return caller; }

int disable_irq_save(void)
{
    return irq_depth++;
}

void restore_irq(int previous)
{
    assert(irq_depth == previous + 1);
    irq_depth = previous;
}

bool rock_iram_drain_playback(void)
{
    assert(!irq_depth && usb_reserved);
    assert(rock_iram_playback_blocked());
    assert(!rock_iram_codec_begin());
    assert(!rock_iram_defer_usb(0));
    stops++;
    playing = false;
    usb_connected |= usb_on_stop;
    if (cached)
    {
        cached = false;
        unloads++;
        rock_iram_codec_end();
    }
    return true;
}

int audio_status(void) { return playing; }

bool track_decode_busy(void) { return track_busy; }
bool usb_inserted(void) { return usb_connected; }
int pcm_current_sink(void) { return PCM_SINK_BUILTIN; }

bool usb_audio_reserve_recording(void)
{
    if (usb_reserved || refuse_reservation)
        return false;
    usb_reserved = true;
    return true;
}

void usb_audio_release_recording(void)
{
    assert(usb_reserved);
    usb_reserved = false;
}

int main(void)
{
    const uintptr_t payload = 0x0bcfc080;
    const uintptr_t source = 0x0bcfd000;
    const uintptr_t file_end = source + 16;
    const uintptr_t data_end = ROCK_IRAM_ORIGIN + 16;
    assert(rock_iram_extent_valid(payload, file_end, source, data_end,
                                  ROCK_IRAM_LIMIT));
    assert(!rock_iram_extent_valid(payload, file_end, payload - 4, data_end,
                                   ROCK_IRAM_LIMIT));
    assert(!rock_iram_extent_valid(payload, file_end, source + 1, data_end,
                                   ROCK_IRAM_LIMIT));
    assert(!rock_iram_extent_valid(payload, file_end - 4, source, data_end,
                                   ROCK_IRAM_LIMIT));
    assert(!rock_iram_extent_valid(payload, file_end, source, data_end,
                                   data_end - 4));
    assert(!rock_iram_extent_valid(payload, file_end, source, data_end,
                                   ROCK_IRAM_LIMIT + 4));
    assert(!rock_iram_extent_valid(payload, file_end, UINTPTR_MAX, data_end,
                                   ROCK_IRAM_LIMIT));

    refuse_reservation = true;
    assert(!rock_iram_acquire() && !stops);
    refuse_reservation = false;
    track_busy = true;
    assert(!rock_iram_acquire() && !stops);
    track_busy = false;

    /* An in-flight codec open can hold the slot before it is cached. */
    assert(rock_iram_codec_begin());
    assert(!rock_iram_codec_begin());
    assert(!rock_iram_acquire());
    assert(!rock_iram_playback_blocked() && !usb_reserved);
    rock_iram_codec_end();

    assert(rock_iram_codec_begin());
    playing = cached = true;
    assert(!rock_iram_acquire());
    assert(playing && cached && !usb_reserved);
    playing = false;
    assert(rock_iram_acquire());
    assert(!playing && !cached && unloads == 1 && usb_reserved);
    assert(!rock_iram_acquire() && !rock_iram_codec_begin());
    assert(rock_iram_playback_blocked());

    intptr_t seqnum;
    assert(!rock_iram_take_usb(&seqnum));
    caller = 2;
    assert(!rock_iram_defer_usb(7));
    caller = 1;
    assert(rock_iram_defer_usb(42));
    assert(!rock_iram_take_usb(&seqnum));
    assert(rock_iram_release());
    assert(rock_iram_take_usb(&seqnum) && seqnum == 42);
    assert(!rock_iram_take_usb(&seqnum) && !usb_reserved);

    usb_on_stop = true;
    assert(!rock_iram_acquire());
    assert(!rock_iram_playback_blocked() && !usb_reserved);
    usb_connected = usb_on_stop = false;

    assert(rock_iram_acquire());
    assert(!rock_iram_release());
    assert(rock_iram_codec_begin());
    rock_iram_codec_end();
}
