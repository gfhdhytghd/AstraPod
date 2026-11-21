/* Foreground tuner capture. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_PITCH_H
#define ASTRAPOD_ROCK_PITCH_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#define ROCK_PITCH_RATE 22050
#define ROCK_PITCH_SAMPLES 4096 /* Interleaved signed 16-bit stereo samples. */

enum rock_pitch_result {
    ROCK_PITCH_STOPPED,
    ROCK_PITCH_WAITING,
    ROCK_PITCH_READY,
    ROCK_PITCH_FAILED,
};

#if defined(ASTRAPOD_ROCK_COMPAT) && defined(IPOD_6G)
#define HAVE_ROCK_PITCH
void rock_pitch_init(void);
void rock_pitch_session_open(void);
void rock_pitch_revoke(void);
bool rock_pitch_start(void);
void rock_pitch_pause(void);
enum rock_pitch_result rock_pitch_read(int16_t (*samples)[ROCK_PITCH_SAMPLES]);
#else
static inline void rock_pitch_init(void) {}
static inline void rock_pitch_session_open(void) {}
static inline void rock_pitch_revoke(void) {}
#endif

#if defined(HAVE_ROCK_PITCH) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#if !defined(HAVE_RECORDING) || !defined(USB_ENABLE_AUDIO)
#error Pitch Detector needs native recording and the shared USB reservation
#endif
#define HAVE_ROCK_PITCH_DMA
void rock_pitch_prepare(unsigned int request);
bool rock_pitch_sink_lock(void);
void rock_pitch_sink_unlock(void);
#endif
#endif
