/* PodLink media import. GPL version 2 or later. */
#ifndef PODLINK_MEDIA_H
#define PODLINK_MEDIA_H

#include "podlink.h"

const char *podlink_media_receive(const struct podlink_message *message,
                                  bool link_idle);
void podlink_media_reset(void);

#endif
