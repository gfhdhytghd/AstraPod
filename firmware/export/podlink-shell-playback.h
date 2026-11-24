/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * PodLink Shell playback requests on EP0.
 ****************************************************************************/
#ifndef PODLINK_SHELL_PLAYBACK_H
#define PODLINK_SHELL_PLAYBACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHELL_PLAYBACK_INDEX 0x4954
#define SHELL_PLAYBACK_EVENT 0x20001u
#define SHELL_PLAYBACK_OUT_SIZE (4 + 260)

struct usb_ctrlrequest;
void shell_playback_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed);
long shell_playback_dispatch(void);

#endif
