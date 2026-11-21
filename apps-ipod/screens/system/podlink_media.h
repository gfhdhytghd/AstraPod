/* PodLink media import. GPL version 2 or later. */
#ifndef PODLINK_MEDIA_H
#define PODLINK_MEDIA_H

#include "podlink.h"

/* Resident PodLink records are limited to 512 bytes by the transport. */
#define MEDIA_REPLY_SIZE 500
#define PODLINK_MEDIA_DIR "/Music/PodLink"

void podlink_media_init(void);
bool podlink_media_enqueue(const struct podlink_message *message);
void podlink_media_disconnect(void);
unsigned int podlink_media_next(char *reply, unsigned int size, bool *refill);
unsigned int podlink_media_next_binary(unsigned char *out, unsigned int size,
                                       bool *refill, uint32_t *raw);
/* True when streaming work took this disk turn. */
bool podlink_media_work(void);

#endif
