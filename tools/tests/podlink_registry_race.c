/* SPDX-License-Identifier: GPL-2.0-or-later
 * Actual Rust service + pthread mutex. No scheduler, USB or device access.
 */
#include "podlink.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t service=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t event=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t irq=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool initialized, entered, resume_callback, unregister_attempted, unregistered;
static int callback_count;
static unsigned char input[256];
static unsigned length,pos;
static int client_snapshot,session_snapshot;
static bool connected_snapshot,running_snapshot;
static uint32_t serial_generation;
bool pfu_init(void) {if(initialized)return false;initialized=true;return true;}
bool pfu_start(void) {return true;}
bool pfu_lock(void) {if(!initialized)return false;assert(!pthread_mutex_lock(&service));return true;}
void pfu_unlock(void) {assert(!pthread_mutex_unlock(&service));}
void pfu_snapshot(int state,int session_state,bool connected,bool running,uint32_t generation)
{client_snapshot=state;session_snapshot=session_state;connected_snapshot=connected;running_snapshot=running;serial_generation=generation;}
int pfu_state(void) {return client_snapshot;}
int pfu_session_state(void) {return session_snapshot;}
bool pfu_session_connected(void) {return connected_snapshot && serial_generation==1;}
bool pfu_host_connected(void) {return running_snapshot && pfu_session_connected();}
int32_t pfu_tick(void) {return 1;}
int32_t pfu_hz(void) {return 100;}
void pfu_wait(int ticks) {(void)ticks;}
void pfu_panic(void) {abort();}
void pfu_redraw(void) {}
bool usb_podlink_open(void) {return true;}
void usb_podlink_close(void) {}
bool usb_podlink_active(void) {return true;}
void usb_podlink_disk(void) {abort();}
bool usb_serial_get_state(uint32_t *generation) {*generation=1;return true;}
bool usb_serial_generation_current(uint32_t generation) {return generation==1;}
int usb_serial_read(unsigned char *out,int size) {unsigned n=length-pos;if(n>(unsigned)size)n=size;memcpy(out,input+pos,n);pos+=n;return n;}
int usb_serial_write(const unsigned char *data,int size) {(void)data;return size;}
void usb_serial_wait(int ticks) {(void)ticks;}
bool remote_log_enabled(void) {return false;}
unsigned remote_log_epoch(void) {return 0;}
unsigned remote_log_read(char *out,unsigned size) {(void)out;(void)size;return 0;}
extern void podlink_rust_step(void);
static bool receive(const struct podlink_message *message)
{
    assert(message->type==PODLINK_BINARY && message->length==4 && !memcmp(message->data,"CAR1",4));
    assert(pthread_mutex_trylock(&service)==EBUSY);
    pthread_mutex_lock(&event);callback_count++;
    assert(!unregistered);
    if(!entered) {entered=true;pthread_cond_broadcast(&condition);while(!resume_callback)pthread_cond_wait(&condition,&event);}
    pthread_mutex_unlock(&event);return true;
}
static void *worker(void *unused) {(void)unused;for(int i=0;i<8;i++)podlink_rust_step();return NULL;}
static void *unregister_consumer(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&event);unregister_attempted=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&event);
    podlink_set_resident(NULL);
    pthread_mutex_lock(&event);unregistered=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&event);
    return NULL;
}
int pfu_irq_save(void) {assert(!pthread_mutex_lock(&irq));return 0;}
void pfu_irq_restore(int level) {(void)level;assert(!pthread_mutex_unlock(&irq));}
int main(void)
{
    podlink_init();
    struct podlink_resident transient={receive,NULL,NULL,NULL};podlink_set_resident(&transient);
    // Invalidate the caller's table immediately. Rust must own a typed copy.
    memset(&transient,0,sizeof transient);
    const unsigned char hello[]={1,0,0,64,0,0};
    length+=podlink_encode(input+length,PODLINK_OPEN,1,hello,6);
    const char *time_record="TS\t1\t2026-11-16T10:00:00\t-18000";
    length+=podlink_encode(input+length,PODLINK_TEXT,2,time_record,strlen(time_record));
    for(unsigned i=0;i<3;i++)length+=podlink_encode(input+length,PODLINK_BINARY,2+i,"CAR1",4);
    pthread_t poller,remover;assert(!pthread_create(&poller,NULL,worker,NULL));
    pthread_mutex_lock(&event);while(!entered)pthread_cond_wait(&condition,&event);pthread_mutex_unlock(&event);
    assert(!pthread_create(&remover,NULL,unregister_consumer,NULL));
    pthread_mutex_lock(&event);while(!unregister_attempted)pthread_cond_wait(&condition,&event);
    assert(!unregistered);
    // These actual exported Rust APIs must finish with the callback holding
    // the service mutex. A regression to with()/mutex_lock deadlocks here.
    assert(podlink_state()==PODLINK_CLOSED);(void)podlink_host_connected();
    char stamp[PODLINK_TIME_RECORD];unsigned long tick;
    assert(podlink_take_time(stamp,&tick));
    assert(!strcmp(stamp,"TS\t1\t2026-11-16T10:00:00\t-18000") && tick==1);
    assert(!podlink_take_time(stamp,&tick));
    resume_callback=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&event);
    assert(!pthread_join(poller,NULL));assert(!pthread_join(remover,NULL));
    int before=callback_count;
    length+=podlink_encode(input+length,PODLINK_BINARY,8,"CAR1",4);
    for(int i=0;i<8;i++)podlink_rust_step();
    assert(callback_count==before && unregistered && before>=1);
    podlink_set_resident(&(struct podlink_resident){NULL,NULL,NULL,NULL});
    podlink_rust_step();podlink_set_resident(NULL);
    assert(podlink_open_agents(NULL));
    assert(!podlink_poll(NULL) && !podlink_take_time(NULL,NULL));
    assert(podlink_send(PODLINK_TEXT,NULL,1)==PODLINK_INVALID);
    assert(podlink_send(PODLINK_TEXT,"x",PODLINK_MAX_MESSAGE+1)==PODLINK_INVALID);
    podlink_close();podlink_shutdown();
    puts("PASS: copied transient registry; NULL fields; callback/unregister share one mutex; no callbacks after unregister; state/host/time reads while callback blocks");
}
