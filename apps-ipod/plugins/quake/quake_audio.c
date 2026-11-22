/* Foreground Quake mixer and owned PCM resampler. GPL-2.0-or-later. */
#include "quakedef.h"

#define SOURCE_RATE 16000
#define OUTPUT_FRAMES 1024
#define SOURCE_FRAMES (OUTPUT_FRAMES * 2)
#define CHANNELS 2

static dma_t dma;
static long audio_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static int16_t source[SOURCE_FRAMES * CHANNELS];
static int16_t output[OUTPUT_FRAMES * CHANNELS];
static unsigned int worker_id;
static unsigned int sink_rate, phase, job_phase;
static int16_t carry_frame[CHANNELS];
static bool have_carry;
static volatile bool closing;
enum audio_job { IDLE, CONVERT, READY };
static volatile enum audio_job job;

static void resample(void *unused)
{
    (void)unused;
    while (!closing && !rb->rock_worker_stopping())
    {
        if (job != CONVERT)
        {
            rb->sleep(1);
            continue;
        }

        unsigned int offset = 0;
        unsigned int position = job_phase;
        for (unsigned int i = 0; i < OUTPUT_FRAMES; i++)
        {
            output[i * CHANNELS] = source[offset * CHANNELS];
            output[i * CHANNELS + 1] = source[offset * CHANNELS + 1];
            position += SOURCE_RATE;
            while (position >= sink_rate)
            {
                position -= sink_rate;
                offset++;
            }
        }
        /* Both participants run on the same cooperative scheduler core. */
        asm volatile ("" ::: "memory");
        job = READY;
    }
}

qboolean SNDDMA_Init(void)
{
    sink_rate = rb->rock_stream_open();
    if (!sink_rate)
        return false;

    closing = false;
    job = IDLE;
    phase = 0;
    have_carry = false;
    worker_id = rb->rock_worker_start(resample, NULL, audio_stack,
                                      sizeof(audio_stack));
    if (!worker_id)
    {
        rb->rock_stream_close();
        return false;
    }

    dma = (dma_t){
        .samplebits = 16, .speed = SOURCE_RATE, .channels = CHANNELS,
        .samples = ARRAYLEN(source), .submission_chunk = 1, .buffer = (byte *)source
    };
    shm = &dma;
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    return (unsigned int)paintedtime * CHANNELS % ARRAYLEN(source);
}

void quake_audio_stop(void)
{
    if (worker_id)
    {
        closing = true;
        rb->rock_worker_join(worker_id);
        worker_id = 0;
    }
    shm = NULL;
    rb->rock_stream_close();
}

void SNDDMA_Shutdown(void)
{
    quake_audio_stop();
}

void quake_audio_pump(void)
{
    if (!worker_id)
        return;

    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        /* Keep the engine's DMA descriptor alive for sound precaching. */
        SNDDMA_Shutdown();
        shm = &dma;
        return;
    }
    if (job == READY && pending < ROCK_STREAM_QUEUE_SIZE)
    {
        if (rb->rock_stream_submit(output, sizeof(output)))
        {
            pending++;
            job = IDLE;
        }
    }
    if (job == IDLE && pending < ROCK_STREAM_QUEUE_SIZE)
    {
        /* Leave one source frame for the next block when the ratio splits it. */
        unsigned int end = phase + OUTPUT_FRAMES * SOURCE_RATE;
        unsigned int advance = end / sink_rate;
        unsigned int carry = (end - SOURCE_RATE) / sink_rate == advance;
        if (have_carry)
        {
            source[0] = carry_frame[0];
            source[1] = carry_frame[1];
        }
        unsigned int keep = have_carry;
        dma.buffer = (byte *)(source + keep * CHANNELS);
        S_PaintChannels(paintedtime + advance + carry - keep);
        dma.buffer = (byte *)source;
        if (carry)
        {
            carry_frame[0] = source[advance * CHANNELS];
            carry_frame[1] = source[advance * CHANNELS + 1];
        }
        have_carry = carry;
        job_phase = phase;
        phase = end % sink_rate;
        asm volatile ("" ::: "memory");
        job = CONVERT;
        rb->yield();
    }
}
