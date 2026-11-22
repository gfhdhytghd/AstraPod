/* Private interfaces for staged MPEGPlayer. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_MPEGPLAYER_COMPAT_H
#define ASTRAPOD_MPEGPLAYER_COMPAT_H

/* Hosted signal stacks have a larger kernel minimum than native stacks. */
#define MPEG_WORKER_STACK_SIZE MAX(DEFAULT_STACK_SIZE + 8*1024, 16*1024)

struct mad_pcm;
struct mpeg_resampler {
    unsigned int phase, rate;
    int32_t last[2];
};

unsigned int mpeg_pcm_rate(void);
bool mpeg_pcm_produce(const struct mad_pcm *pcm, uint32_t *pts,
                      struct mpeg_resampler *resampler);
bool mpeg_pcm_pump(void);
void mpeg_stream_poll(void);
void mpeg_yuv_blit(uint8_t *const *planes, int sx, int sy, int stride,
                   int x, int y, int width, int height);

#endif
