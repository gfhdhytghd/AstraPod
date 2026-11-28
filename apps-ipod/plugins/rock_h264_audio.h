/* Foreground AAC decoder lease. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_H264_AUDIO_H
#define ASTRAPOD_ROCK_H264_AUDIO_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct codec_api;

struct rock_h264_audio_progress {
    uint32_t media_base_ms, rate, delivered, completed, dma_frames;
    uint32_t underrun_frames, queued;
};

struct rock_h264_audio_api {
    bool (*open)(void *pcm, size_t bytes, uint32_t rate);
    bool (*start)(void (*decode)(void));
    bool (*running)(void);
    int (*decode_aac)(struct codec_api *api);
    size_t (*write)(const int16_t *pcm, size_t frames);
    uint32_t (*buffered)(void);
    uint32_t (*clock_ms)(void);
    bool (*pause)(bool paused);
    bool (*seek)(uint32_t time_ms);
    void (*close)(void);
    bool (*progress)(struct rock_h264_audio_progress *progress);
};

#ifdef HAVE_HW_H264
extern const struct rock_h264_audio_api rock_h264_audio_api;
#define ROCK_H264_AUDIO_API (&rock_h264_audio_api)
void rock_h264_audio_begin(void *scratch, size_t size);
void rock_h264_audio_revoke(void);
#else
#define ROCK_H264_AUDIO_API NULL
static inline void rock_h264_audio_begin(void *scratch, size_t size)
{
    (void)scratch;
    (void)size;
}
static inline void rock_h264_audio_revoke(void) {}
#endif

#endif
