/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ROCK_PODLINK_PORT_H
#define ROCK_PODLINK_PORT_H

#include "rock_podlink.h"

void rock_podlink_init(void);
void rock_podlink_storage(bool exclusive);
bool rock_podlink_work(void);
bool rock_podlink_has_deadline(void);

/* The native port uses short IRQ critical sections on ipod6g. The disk
 * worker is the sole caller of work() and owns all file descriptors. */
int apl_lock(void);
void apl_unlock(int level);
void apl_wake(void);
unsigned int apl_start_worker(void);
uint32_t apl_now(void);
unsigned int apl_hz(void);
int apl_file_size(int fd, uint32_t *size);

#endif
