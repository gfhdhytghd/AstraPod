/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "system.h"
#include "usb.h"
#include "file.h"
#include "rock_podlink_port.h"

static struct event_queue queue;
static long stack[DEFAULT_STACK_SIZE * 2 / sizeof(long)];
static bool posted;
enum { WORK_READY = 1 };

int apl_lock(void) { return disable_irq_save(); }
void apl_unlock(int level) { restore_irq(level); }
uint32_t apl_now(void) { return current_tick; }
unsigned int apl_hz(void) { return HZ; }

void apl_wake(void)
{
    int irq = apl_lock();
    if (!posted)
    {
        posted = true;
        queue_post(&queue, WORK_READY, 0);
    }
    apl_unlock(irq);
}

int apl_file_size(int fd, uint32_t *size)
{
    off_t len = ffilesize(fd);
    if (len < 0)
        return ROCK_PODLINK_IO;
    if ((uint64_t)len > ROCK_PODLINK_FILE_MAX)
        return ROCK_PODLINK_LIMIT;
    *size = len;
    return ROCK_PODLINK_OK;
}

static void apl_worker(void)
{
    struct queue_event event;
    bool ready = false;
    while (true)
    {
        if (ready || rock_podlink_has_deadline())
            queue_wait_w_tmo(&queue, &event, ready ? 0 : MAX(1, HZ / 10));
        else
            queue_wait(&queue, &event);
        if (event.id == SYS_USB_CONNECTED)
        {
            rock_podlink_storage(true);
            usb_acknowledge(SYS_USB_CONNECTED_ACK, event.data);
        }
        else if (event.id == SYS_USB_DISCONNECTED)
            rock_podlink_storage(false);
        else if (event.id == WORK_READY)
        {
            int irq = apl_lock();
            posted = false;
            apl_unlock(irq);
        }
        ready = rock_podlink_work();
        if (ready)
            yield();
    }
}

unsigned int apl_start_worker(void)
{
    queue_init(&queue, true);
    unsigned int id = create_thread(apl_worker, stack, sizeof(stack), 0,
                                    "plugin PodLink"
                                    IF_PRIO(, PRIORITY_BACKGROUND)
                                    IF_COP(, CPU));
    if (!id)
        queue_delete(&queue);
    return id;
}
