/* MPEGPlayer's foreground bridge. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_MPEG_H
#define ASTRAPOD_ROCK_MPEG_H

#include "kernel.h"

struct rock_mpeg_api {
    bool (*set_drain)(void (*drain)(void));
    unsigned int (*start_worker)(void (*run)(void *), void *arg,
                                 void *stack, size_t bytes);
    __typeof__(queue_init) *queue_init;
    __typeof__(queue_enable_queue_send) *queue_enable_queue_send;
    __typeof__(queue_empty) *queue_empty;
    __typeof__(queue_post) *queue_post;
    __typeof__(queue_send) *queue_send;
    __typeof__(queue_reply) *queue_reply;
    __typeof__(queue_wait) *queue_wait;
    __typeof__(queue_wait_w_tmo) *queue_wait_w_tmo;
    __typeof__(mutex_init) *mutex_init;
    __typeof__(mutex_lock) *mutex_lock;
    __typeof__(mutex_unlock) *mutex_unlock;
    void (*storage_idle)(void);
    void (*storage_keep_active)(void);
};

#endif
