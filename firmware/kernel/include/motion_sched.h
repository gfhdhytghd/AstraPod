/* AstraPod iPod Classic scheduling policy. GPL version 2+. */
#ifndef ASTRA_MOTION_SCHED_H
#define ASTRA_MOTION_SCHED_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

struct mutex;

#if defined(IPOD_6G) && defined(HAVE_PRIORITY_SCHEDULING) && !defined(BOOTLOADER)
#define HAVE_MOTION_SCHED
#define PRIORITY_MOTION_UI 14
#define PRIORITY_MOTION_AUDIO 13

void motion_sched_publish(bool active, long deadline);
bool motion_sched_active(void);
bool motion_sched_defer_io(void);
bool motion_sched_background_io(void);
void motion_sched_set_disk_mutex(struct mutex *mutex);

struct motion_sched_wait
{
    uint32_t runnable, inherited, other;
    uintptr_t lock_site;
};
void motion_sched_watch(unsigned thread_id);
struct motion_sched_wait motion_sched_get_wait(void);
#endif

#endif
