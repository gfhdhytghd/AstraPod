/* Firmware boundaries for the opt-in codec IRAM lease. GPL version 2+. */
#include "audio.h"
#include "audio/codec_thread.h"
#include "audio/track_decode.h"
#include "button.h"
#include "codecs.h"
#include "plugins/rock_iram.h"
#include "system/shutdown.h"

int __real_codec_load_file(const char *name, struct codec_api *api);
int __real_codec_close(void);
void __real_audio_play(unsigned long elapsed, unsigned long offset);
long __real_default_event_handler_ex(long event, void (*cleanup)(void *),
                                     void *parameter);

/* A failed codec entry can leave a handle that still uses IRAM. */
static int finish_codec_load(int result)
{
    if (result < 0)
    {
        __real_codec_close();
        rock_iram_codec_end();
    }
    return result;
}

int __wrap_codec_load_file(const char *name, struct codec_api *api)
{
    if (!rock_iram_codec_begin())
        return CODEC_ERROR;
    return finish_codec_load(__real_codec_load_file(name, api));
}

#ifdef HAVE_CODEC_BUFFERING
int __real_codec_load_buf(int handle, struct codec_api *api);

int __wrap_codec_load_buf(int handle, struct codec_api *api)
{
    if (!rock_iram_codec_begin())
        return CODEC_ERROR;
    return finish_codec_load(__real_codec_load_buf(handle, api));
}
#endif

int __wrap_codec_close(void)
{
    int result = __real_codec_close();
    rock_iram_codec_end();
    return result;
}

bool rock_iram_drain_playback(void)
{
    /* A paused track still owns the codec. Only unload a cached idle one. */
    if (audio_status() || track_decode_busy())
        return false;
    if (codec_loaded() != AFMT_UNKNOWN)
        codec_unload();
    return !audio_status() && !track_decode_busy();
}

void __wrap_audio_play(unsigned long elapsed, unsigned long offset)
{
    if (!rock_iram_playback_blocked())
        __real_audio_play(elapsed, offset);
}

long __wrap_default_event_handler_ex(long event, void (*cleanup)(void *),
                                     void *parameter)
{
    if (event == SYS_USB_CONNECTED && rock_iram_defer_usb(button_get_data()))
    {
        if (cleanup)
            cleanup(parameter);
        return SYS_USB_CONNECTED;
    }
    return __real_default_event_handler_ex(event, cleanup, parameter);
}

long __wrap_default_event_handler(long event)
{
    return __wrap_default_event_handler_ex(event, NULL, NULL);
}

void rock_iram_finish_usb(void)
{
    /* Repost the original sequence to the native UI after plugin teardown. */
    intptr_t sequence;
    if (rock_iram_take_usb(&sequence))
        button_queue_post(SYS_USB_CONNECTED, sequence);
}
