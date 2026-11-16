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

static struct mutex service_mutex;
static bool initialized;
static int client_state, session_state;
static bool connected, host_running;
static uint32_t serial_generation;
static long service_stack[DEFAULT_STACK_SIZE * 2 / sizeof(long)];
/* Rust separately asserts its inbox+welcome+resident storage <= 51000. */
_Static_assert(51000 + sizeof(service_stack) < 64 * 1024,
               "resident Rust PodLink RAM budget");
_Static_assert(sizeof(struct podlink_message) == 16, "Rust message ABI");
_Static_assert(sizeof(struct podlink_resident) == 12, "Rust registry ABI");
_Static_assert(sizeof(struct podlink_parser) == PODLINK_FRAME_MAX + 8,
               "Rust parser ABI");
extern void podlink_rust_step(void);
static void service_thread(void)
{
    while (true) {
        podlink_rust_step();
        sleep(MAX(1, HZ / 50));
    }
}
bool pfu_init(void)
{
    if (initialized) return false; /* Boot/UI thread owns initialization. */
    mutex_init(&service_mutex);
    initialized = true;
    return true;
}
bool pfu_start(void)
{
    return create_thread(service_thread, service_stack, sizeof(service_stack), 0,
                         "podlink" IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU)) != 0;
}
bool pfu_lock(void)
{
    if (!initialized) return false;
    mutex_lock(&service_mutex);
    return true;
}
void pfu_unlock(void) { mutex_unlock(&service_mutex); }
void pfu_snapshot(int state, int session, bool host, bool running, uint32_t generation)
{
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
int32_t pfu_tick(void) { return (int32_t)current_tick; }
int32_t pfu_hz(void) { return HZ; }
void pfu_wait(int ticks) { sleep(MIN(ticks, MAX(1, HZ / 50))); }
void pfu_redraw(void) { button_queue_post(BUTTON_REDRAW, 0); }
void pfu_panic(void) { panicf("Rust PodLink panic"); }

int pfu_irq_save(void) { return disable_irq_save(); }
void pfu_irq_restore(int level) { restore_irq(level); }
