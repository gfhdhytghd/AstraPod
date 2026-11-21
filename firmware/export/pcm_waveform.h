/* Native playback snapshot for foreground plugins. GNU GPL v2 or later. */
#ifndef PCM_WAVEFORM_H
#define PCM_WAVEFORM_H

#include <stddef.h>
#include <stdint.h>

#define PCM_WAVEFORM_FRAMES 1024

/* The caller serializes reads and supplies room for PCM_WAVEFORM_FRAMES
 * interleaved stereo frames. Returns the number of frames copied. */
size_t mixer_waveform_read(int16_t *samples, unsigned int window_ms);

#endif
