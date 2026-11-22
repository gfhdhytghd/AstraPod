/* Foreground lease on the codec IRAM window. GNU GPL version 2 or later. */
#include "plugins/rock_iram.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "audio/track_decode.h"
#include "pcm.h"
#include "pcm_sink.h"
#include "usb.h"
#include "usbstack/usb_audio.h"

enum rock_iram_state { IRAM_FREE, IRAM_DRAINING, IRAM_HELD };
static enum rock_iram_state state;
static bool codec_busy;
static unsigned int foreground;
static bool usb_pending;
static intptr_t usb_sequence;

bool rock_iram_codec_begin(void)
{
    int irq = disable_irq_save();
    bool available = state == IRAM_FREE && !codec_busy;
    if (available)
        codec_busy = true;
    restore_irq(irq);
    return available;
}

void rock_iram_codec_end(void)
{
    int irq = disable_irq_save();
    codec_busy = false;
    restore_irq(irq);
}

bool rock_iram_playback_blocked(void)
{
    int irq = disable_irq_save();
    bool blocked = state != IRAM_FREE;
    restore_irq(irq);
    return blocked;
}

static bool rock_iram_media_available(void)
{
    return !usb_inserted() && !track_decode_busy() && !audio_status() &&
           pcm_current_sink() == PCM_SINK_BUILTIN;
}

bool rock_iram_acquire(void)
{
    if (!rock_iram_media_available())
        return false;
#ifdef USB_ENABLE_AUDIO
    /* Also excludes native recording and new USB streaming alternates. */
    if (!usb_audio_reserve_recording())
        return false;
#endif

    int irq = disable_irq_save();
    if (state != IRAM_FREE)
    {
        restore_irq(irq);
#ifdef USB_ENABLE_AUDIO
        usb_audio_release_recording();
#endif
        return false;
    }
    state = IRAM_DRAINING;
    restore_irq(irq);

    if (rock_iram_drain_playback() && rock_iram_media_available())
    {
        irq = disable_irq_save();
        if (!codec_busy && !usb_inserted())
        {
            foreground = thread_self();
            state = IRAM_HELD;
            restore_irq(irq);
            return true;
        }
        restore_irq(irq);
    }

    rock_iram_release();
    return false;
}

bool rock_iram_release(void)
{
#ifdef USB_ENABLE_AUDIO
    usb_audio_release_recording();
#endif
    int irq = disable_irq_save();
    foreground = 0;
    state = IRAM_FREE;
    restore_irq(irq);
    return usb_pending;
}

/* The foreground keeps the USB sequence until it has joined its workers
 * and released the arena. Only then may the UI acknowledge storage. */
bool rock_iram_defer_usb(intptr_t seqnum)
{
    if (state != IRAM_HELD || foreground != thread_self())
        return false;
    usb_sequence = seqnum;
    usb_pending = true;
    return true;
}

bool rock_iram_take_usb(intptr_t *seqnum)
{
    if (state != IRAM_FREE || !usb_pending)
        return false;
    *seqnum = usb_sequence;
    usb_pending = false;
    return true;
}
