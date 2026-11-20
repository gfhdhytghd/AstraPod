/* PodLink media import. GPL version 2 or later. */
#ifndef PODLINK_MEDIA_H
#define PODLINK_MEDIA_H

#include "podlink.h"

void podlink_media_init(void);
bool podlink_media_enqueue(const struct podlink_message *message);
void podlink_media_disconnect(void);
unsigned int podlink_media_next(char *reply, unsigned int size);
void podlink_media_work(void);

#endif
