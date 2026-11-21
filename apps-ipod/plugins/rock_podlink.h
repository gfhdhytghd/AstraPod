/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCK_PODLINK_H
#define ROCK_PODLINK_H

#include <stdbool.h>
#include <stdint.h>

#define ROCK_PODLINK_MESSAGE_MAX 2048
#define ROCK_PODLINK_FILE_MAX (64u * 1024u * 1024u)

enum rock_podlink_result {
    ROCK_PODLINK_OK,
    ROCK_PODLINK_NO_COMPANION,
    ROCK_PODLINK_BUSY,
    ROCK_PODLINK_INVALID,
    ROCK_PODLINK_IO,
    ROCK_PODLINK_TIMEOUT,
    ROCK_PODLINK_CANCELLED,
    ROCK_PODLINK_LINK_LOST,
    ROCK_PODLINK_LIMIT,
    ROCK_PODLINK_UNSUPPORTED, /* SDK only, never transmitted */
};

enum rock_podlink_event_type {
    ROCK_PODLINK_OPENED = 1,
    ROCK_PODLINK_CLOSED,
    ROCK_PODLINK_TEXT,
    ROCK_PODLINK_BINARY,
    ROCK_PODLINK_PROGRESS,
    ROCK_PODLINK_COMPLETE,
};

struct rock_podlink_event {
    enum rock_podlink_event_type type;
    enum rock_podlink_result result;
    uint32_t length; /* Message bytes, excluding any terminator */
    uint32_t bytes, total, crc32; /* File progress or completion */
    unsigned char data[ROCK_PODLINK_MESSAGE_MAX];
};

typedef void (*rock_podlink_progress_fn)(const struct rock_podlink_event *event,
                                         void *context);

/* Commands return 0 on enqueue or minus a result code. Poll returns the
 * event type, 0 while pending, or minus a result code. It never sleeps.
 * A positive timeout arms a deadline for repeated polls. */
int rock_podlink_open(const char *name);
void rock_podlink_close(void);
int rock_podlink_send(bool binary, const void *data, unsigned int length);
int rock_podlink_poll(struct rock_podlink_event *event, unsigned int timeout_ms,
                      rock_podlink_progress_fn progress, void *context);
int rock_podlink_push(const char *path, const char *object);
int rock_podlink_pull(const char *path, const char *object);
int rock_podlink_cancel(void);

#endif
