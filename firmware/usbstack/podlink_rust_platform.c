/* SPDX-License-Identifier: GPL-2.0-or-later
 * Rockbox scheduling/IRQ adapter only. No protocol state or callback dispatch.
 */
#include "config.h"
#include "podlink.h"
#include "usb.h"
#include "usb_serial.h"
#include "kernel.h"
#include "thread.h"
#include "system.h"
#include "panic.h"
#include "button.h"
#include "crumbs.h"

static struct mutex service_mutex;
static bool initialized;
static int client_state, session_state;
static bool connected, host_running;
static uint32_t serial_generation;
/* The stack grows down towards a checked guard, never into the mutex or the
 * snapshots: an overflow is a panic, not a silently wedged lock. */
#define GUARD_WORDS 16
#define GUARD 0x504c4b47u
static struct
{
    uint32_t guard[GUARD_WORDS];
    long stack[DEFAULT_STACK_SIZE * 2 / sizeof(long)];
} worker;
/* Rust separately asserts its inbox+welcome+resident storage <= 51000. */
_Static_assert(51000 + sizeof(worker) < 64 * 1024,
               "resident Rust PodLink RAM budget");
_Static_assert(sizeof(struct podlink_message) == 16, "Rust message ABI");
_Static_assert(sizeof(struct podlink_resident) == 16, "Rust registry ABI");
_Static_assert(sizeof(struct podlink_parser) == PODLINK_FRAME_MAX + 8,
               "Rust parser ABI");
extern void podlink_rust_step(void);
static unsigned int worker_id;
static void service_thread(void)
{
    uint32_t steps = 0;
    crumb(CR_PL_WORKER_START, 0);
    while (true) {
        crumb_status(CS_PL_WORKER, ++steps << 1 | 1);
        podlink_rust_step();
        crumb_status(CS_PL_WORKER, steps << 1);
        for (int i = 0; i < GUARD_WORDS; i++)
            if (worker.guard[i] != GUARD) panicf("PodLink stack overflow");
        /* A latched wake may make the wait return without scheduling. */
        yield();
        usb_serial_wait_activity(MAX(1, HZ / 50));
    }
}
bool pfu_init(void)
{
    if (initialized) return false; /* Boot/UI thread owns initialization. */
    for (int i = 0; i < GUARD_WORDS; i++) worker.guard[i] = GUARD;
    mutex_init(&service_mutex);
    initialized = true;
    return true;
}
bool pfu_start(void)
{
    worker_id = create_thread(service_thread, worker.stack,
                              sizeof(worker.stack), 0, "podlink"
                              IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
    return worker_id != 0;
}
bool pfu_lock(void)
{
    if (!initialized) return false;
    /* Every PodLink call that takes the service lock passes here. The
     * worker's own steps are counted in CS_PL_WORKER; a UI call is an entry
     * (the Rust caller's address: a poll loop collapses into one). */
    uint32_t caller = (uint32_t)__builtin_return_address(0);
#ifdef HAVE_CRUMBS
    if (thread_self() != worker_id)
        crumb(CR_PL_API, caller);
#endif
    mutex_lock(&service_mutex);
    crumb_status(CS_PL_LOCK, caller);
    return true;
}
void pfu_unlock(void)
{
    crumb_status(CS_PL_LOCK, 0);
    mutex_unlock(&service_mutex);
}
void pfu_snapshot(int state, int session, bool host, bool running, uint32_t generation)
{
    if (state != client_state || session != session_state)
        crumb(CR_PL_STATE, (uint32_t)state << 8 | (uint32_t)session);
    int level = disable_irq_save();
    client_state = state; session_state = session;
    connected = host; host_running = running; serial_generation = generation;
    restore_irq(level);
}
int pfu_state(void)
{
    int level = disable_irq_save();
    int result = client_state;
    restore_irq(level);
    return result;
}
int pfu_session_state(void) { return session_state; }
bool pfu_session_connected(void)
{
    int level = disable_irq_save();
    bool result = connected && usb_serial_generation_current(serial_generation);
    restore_irq(level);
    return result;
}
bool pfu_host_connected(void)
{
    int level = disable_irq_save();
    bool result = host_running && usb_podlink_active() && pfu_session_connected();
    restore_irq(level);
    return result;
}
void podlink_wake(void) { usb_serial_wake(); }
int32_t pfu_tick(void) { return (int32_t)current_tick; }
int32_t pfu_hz(void) { return HZ; }
void pfu_wait(int ticks) { sleep(MIN(ticks, MAX(1, HZ / 50))); }
void pfu_redraw(void) { button_queue_post(BUTTON_REDRAW, 0); }
void pfu_panic(void)
{
    crumb(CR_RUST_PANIC, (uint32_t)__builtin_return_address(0));
    panicf("Rust PodLink panic");
}

int pfu_irq_save(void)
{
    int level = disable_irq_save();
    crumb_status(CS_PL_IRQ, (uint32_t)__builtin_return_address(0) | 1);
    return level;
}
void pfu_irq_restore(int level)
{
    crumb_status(CS_PL_IRQ, (uint32_t)__builtin_return_address(0) & ~1u);
    restore_irq(level);
}
