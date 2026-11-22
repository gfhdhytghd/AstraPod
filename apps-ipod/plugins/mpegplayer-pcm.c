/* MPEGPlayer's timestamped producer queue. GNU GPL version 2 or later. */
#include "plugin.h"
#include "mpegplayer.h"
#include "codecs/libmad/mad.h"
#include "mpegplayer-compat.h"

#define PCM_BLOCKS 4
#define PCM_FRAME_MAX (1152 * 6) /* 8 kHz source at a 48 kHz sink */
#define PCM_SUBMIT_FRAMES 512

struct mpeg_pcm_block {
    uint32_t pts;
    unsigned int frames;
    int16_t samples[PCM_FRAME_MAX][2];
};

static struct mpeg_pcm_block *blocks;
static struct mutex pcm_lock;
static unsigned int front, count, offset;
static unsigned int sink_rate;
static int pending;
static uint32_t start, base, submitted_until;
static long anchor;
static bool running, opened;

static uint32_t clock_now(void)
{
    return base + (running ?
        (uint64_t)((unsigned long)*rb->current_tick -
                   (unsigned long)anchor) * sink_rate / HZ :
        0);
}

unsigned int mpeg_pcm_rate(void)
{
    return sink_rate;
}

bool pcm_output_init(void)
{
    rb->mpeg->mutex_init(&pcm_lock);
    blocks = mpeg_malloc(sizeof(*blocks) * PCM_BLOCKS, MPEG_ALLOC_PCMOUT);
    if (!blocks)
        return false;

    sink_rate = rb->rock_stream_open();
    opened = sink_rate != 0;
    return opened;
}

void pcm_output_exit(void)
{
    rb->rock_stream_close();
    opened = false;
}

void pcm_output_flush(void)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    rb->rock_stream_close();
    opened = rb->rock_stream_open() == sink_rate;
    front = count = offset = pending = 0;
    submitted_until = clock_now();
    rb->mpeg->mutex_unlock(&pcm_lock);
}

void pcm_output_set_clock(uint32_t time)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    start = base = submitted_until = time;
    anchor = *rb->current_tick;
    rb->mpeg->mutex_unlock(&pcm_lock);
}

uint32_t pcm_output_get_ticks(uint32_t *first)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    uint32_t now = clock_now();
    if (first)
        *first = start;
    rb->mpeg->mutex_unlock(&pcm_lock);
    return now;
}

uint32_t pcm_output_get_clock(void)
{
    return pcm_output_get_ticks(NULL);
}

void pcm_output_play_pause(bool play)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    if (play != running)
    {
        base = clock_now();
        anchor = *rb->current_tick;
        running = play;
        if (opened)
            opened = rb->rock_stream_pause(!play);
    }
    rb->mpeg->mutex_unlock(&pcm_lock);
}

void pcm_output_stop(void)
{
    pcm_output_play_pause(false);
    pcm_output_flush();
    pcm_output_set_clock(0);
}

bool pcm_output_empty(void)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    bool empty = !opened || (!count && pending <= 0);
    rb->mpeg->mutex_unlock(&pcm_lock);
    return empty;
}

static int16_t mpeg_sample(mad_fixed_t current, mad_fixed_t previous,
                          unsigned int frac)
{
    int value = (MIN(MAX(current, -MAD_F_ONE), MAD_F_ONE - 1) >>
                 (MAD_F_FRACBITS + 1 - 16));
    int last = (MIN(MAX(previous, -MAD_F_ONE), MAD_F_ONE - 1) >>
                (MAD_F_FRACBITS + 1 - 16));
    return last + (value - last) * (int)frac / 256;
}

/* Audio worker owns the uncommitted tail. The manager pauses and
 * acknowledges that worker before resetting the queue on seek. */
bool mpeg_pcm_produce(const struct mad_pcm *pcm, uint32_t *pts,
                      struct mpeg_resampler *resampler)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    unsigned int tail = (front + count) % PCM_BLOCKS;
    bool full = count == PCM_BLOCKS;
    rb->mpeg->mutex_unlock(&pcm_lock);
    if (full)
        return false;

    struct mpeg_pcm_block *block = &blocks[tail];
    unsigned int frames = 0;
    unsigned int rate = pcm->samplerate;
    if (rate < 8000 || rate > 48000 || pcm->length > 1152 ||
        (pcm->channels != 1 && pcm->channels != 2))
        return true;

    if (rate != resampler->rate)
    {
        resampler->rate = rate;
        resampler->phase = rate;
        resampler->last[0] = pcm->samples[0][0];
        resampler->last[1] = pcm->samples[pcm->channels - 1][0];
    }

    unsigned int phase = resampler->phase;
    for (unsigned int i = 0; i < pcm->length; ++i)
    {
        mad_fixed_t left = pcm->samples[0][i];
        mad_fixed_t right = pcm->samples[pcm->channels - 1][i];
        while (phase >= rate)
        {
            unsigned int frac = (phase - rate) * 256 / sink_rate;
            block->samples[frames][0] =
                mpeg_sample(left, resampler->last[0], 256 - frac);
            block->samples[frames++][1] =
                mpeg_sample(right, resampler->last[1], 256 - frac);
            phase -= rate;
        }
        phase += sink_rate;
        resampler->last[0] = left;
        resampler->last[1] = right;
    }
    resampler->phase = phase;

    if (frames)
    {
        block->pts = *pts;
        block->frames = frames;
        *pts += frames;
        rb->mpeg->mutex_lock(&pcm_lock);
        count++;
        rb->mpeg->mutex_unlock(&pcm_lock);
    }
    return true;
}

/* Only the foreground submits PCM. Compare PTS with the bounded end of
 * queued audio, leaving gaps silent and trimming samples that arrive late. */
bool mpeg_pcm_pump(void)
{
    rb->mpeg->mutex_lock(&pcm_lock);
    pending = opened ? rb->rock_stream_pending() : -1;
    if (pending < 0)
        opened = false;
    uint32_t now = clock_now();
    if (!pending)
        submitted_until = now;

    while (opened && running && count && pending < ROCK_STREAM_QUEUE_SIZE)
    {
        struct mpeg_pcm_block *block = &blocks[front];
        int32_t gap = block->pts + offset - submitted_until;
        if (gap > (int32_t)(sink_rate / HZ))
            break;

        if (gap < 0)
            offset += MIN((uint32_t)-(int64_t)gap, block->frames - offset);

        unsigned int frames = MIN(block->frames - offset, PCM_SUBMIT_FRAMES);
        if (frames)
        {
            if (!rb->rock_stream_submit(block->samples[offset], frames * 4))
                break;
            submitted_until = block->pts + offset + frames;
            offset += frames;
            pending++;
        }
        if (offset == block->frames)
        {
            front = (front + 1) % PCM_BLOCKS;
            count--;
            offset = 0;
        }
    }
    rb->mpeg->mutex_unlock(&pcm_lock);
    return opened;
}
