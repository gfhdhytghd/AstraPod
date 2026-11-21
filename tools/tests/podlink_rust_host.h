/* SPDX-License-Identifier: GPL-2.0-or-later
 * RAM-only platform adapter for the unchanged C assertion streams.
 */
#include "podlink_session.h"
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#include <stdlib.h>
static struct mutex service_mutex;
static bool pfu_initialized;
static int pfu_client_state, pfu_wire_state;
static bool pfu_connected, pfu_running;
static uint32_t pfu_generation;
static const unsigned char hello[] = {1,0,0,64,0,0};
bool pfu_init(void) { if(pfu_initialized)return false;pfu_initialized=true;return true; }
bool pfu_start(void) { return true; }
bool pfu_lock(void) { mutex_lock(&service_mutex);return true; }
void pfu_unlock(void) { mutex_unlock(&service_mutex); }
void pfu_snapshot(int state,int session,bool connected,bool running,uint32_t generation)
{ pfu_client_state=state;pfu_wire_state=session;pfu_connected=connected;pfu_running=running;pfu_generation=generation; }
int pfu_state(void) { return pfu_client_state; }
int pfu_session_state(void) { return pfu_wire_state; }
bool pfu_session_connected(void) {return pfu_connected && usb_serial_generation_current(pfu_generation);}
bool pfu_host_connected(void) {return pfu_running && usb_podlink_active() && pfu_session_connected();}
int32_t pfu_tick(void) {return (int32_t)current_tick;}
int32_t pfu_hz(void) {return HZ;}
void pfu_wait(int ticks) {current_tick+=MIN(ticks,MAX(1,HZ/50));}
void podlink_wake(void) {}
void pfu_panic(void) {fputs("Rust PodLink panic\n",stderr);abort();}
#ifdef PODLINK_SERVICE_TEST
extern void button_queue_post(long id,intptr_t data);
void pfu_redraw(void) {button_queue_post(0x20000000,0);}
#else
void pfu_redraw(void) {}
#endif
extern unsigned podlink_rust_debug(unsigned id);
extern void podlink_rust_set_peer(unsigned n);
extern void podlink_rust_log(unsigned char *out);
extern void podlink_rust_invalidate(void);
extern void podlink_rust_step(void);

int pfu_irq_save(void) { return disable_irq_save(); }
void pfu_irq_restore(int level) { restore_irq(level); }
