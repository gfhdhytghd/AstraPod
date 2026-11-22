/* Foreground PCM stream ownership. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_STREAM_H
#define ASTRAPOD_ROCK_STREAM_H

#include <stdbool.h>
#include <stddef.h>

#define ROCK_STREAM_BLOCK_BYTES 4096
#define ROCK_STREAM_QUEUE_SIZE 2

/* PCM is signed 16-bit stereo in native byte order at the returned rate. */
unsigned int rock_stream_open(void);
/* Return outstanding blocks, including the one playing, or -1 on revoke. */
int rock_stream_pending(void);
/* Copy one complete block. Return false when full, invalid or revoked. */
bool rock_stream_submit(const void *samples, size_t size);
bool rock_stream_pause(bool paused);
void rock_stream_close(void);

/* The firmware grants a new opportunity only when a foreground image starts. */
void rock_stream_session_open(void);
void rock_stream_revoke(void);

#endif
