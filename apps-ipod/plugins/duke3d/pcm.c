/* Run stock MultiVoc on the foreground thread. GNU GPL version 2 or later. */
#include "duke-build.h"
#include "dsl.h"

extern int MV_MixPage;

static unsigned int sink_rate;
static bool tried_sink;
static void (*mix_page)(void);
static char *mix_data;
static size_t page_bytes;
static unsigned long last_tick;
static unsigned char block[ROCK_STREAM_BLOCK_BYTES];

unsigned int duke_pcm_rate(void)
{
    if (!tried_sink)
    {
        tried_sink = true;
        sink_rate = rb->rock_stream_open();
    }
    return sink_rate ? sink_rate : 44100;
}

int DSL_Init(void)
{
    return DSL_Ok;
}

char *DSL_ErrorString(int error)
{
    return error == DSL_Ok ? "Duke3D PCM ready" : "Invalid Duke3D mixer format";
}

int DSL_BeginBufferedPlayback(char *data, int bytes, int divisions,
                              unsigned int rate, int mode,
                              void (*callback)(void))
{
    if (!callback || !data || divisions <= 0 || bytes <= 0 ||
        bytes % divisions || bytes / divisions > ROCK_STREAM_BLOCK_BYTES ||
        (bytes / divisions) % 4 || rate != duke_pcm_rate() || mode != STEREO_16BIT)
        return DSL_Error;

    mix_data = data;
    page_bytes = bytes / divisions;
    mix_page = callback;
    return DSL_Ok;
}

void duke_pcm_pump(void)
{
    if (!mix_page)
        return;

    unsigned long now = *rb->current_tick;
    if (last_tick == now)
        return;
    last_tick = now;

    int pending = sink_rate ? rb->rock_stream_pending() : -1;
    if (pending < 0)
    {
        /* Advance voices in silent mode so they release their cache locks. */
        mix_page();
        return;
    }

    size_t bytes = sizeof(block) - sizeof(block) % page_bytes;
    for (int queued = pending; queued < ROCK_STREAM_QUEUE_SIZE; queued++)
    {
        for (size_t offset = 0; offset < bytes; offset += page_bytes)
        {
            mix_page();
            rb->memcpy(block + offset, mix_data + MV_MixPage * page_bytes,
                       page_bytes);
        }
        if (!rb->rock_stream_submit(block, bytes))
            break;
    }
}

void DSL_StopPlayback(void)
{
    mix_page = NULL;
    if (sink_rate)
        rb->rock_stream_close();
    sink_rate = 0;
    tried_sink = false;
}

void DSL_Shutdown(void)
{
    DSL_StopPlayback();
}

unsigned int DSL_GetPlaybackRate(void)
{
    return duke_pcm_rate();
}

uint32_t DisableInterrupts(void)
{
    return 0;
}

void RestoreInterrupts(uint32_t flags)
{
    (void)flags;
}
