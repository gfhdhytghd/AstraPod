"""Production worker decisions: cold Cider-only boot and dirty USB art flush.
The file-backed cs_load/cs_put implementation is tested by cider_store_test.c.
argv[2] optionally selects the original source for preserved baseline-red.
"""
from pathlib import Path
import re,subprocess,sys
r=Path(__file__).resolve().parents[2];src=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else r;o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
def fn(s,n):
 m=re.search(r'(?m)^static [\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
s=(src/'apps-ipod/screens/playback/cider.c').read_text();worker=fn(s,'disk_worker')
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
static void podlink_media_disconnect(void) {}
static bool podlink_media_work(void) { return false; }
#include "screens/playback/cider_store.h"
#include "screens/playback/cider_model.h"
#define HZ 100
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define TIME_AFTER(a,b) ((long)((a)-(b))>0)
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define SYS_USB_CONNECTED 1
#define SYS_USB_DISCONNECTED 2
#define SYS_USB_CONNECTED_ACK 3
struct queue_event {int id,data;};
static struct cider_store store;
static struct cider_cache cache;
static struct cider_transfer transfer;
static struct cider_state state;
static struct {char key[65];unsigned char tier;} fills[8];
static struct cider_store_stats diag;
static bool disk_lookup,list_req,list_ready,load_requested,build_on;
static int disk_queue,cider_mutex;static unsigned nfills;
static long current_tick=1000,art_used_at,dirty_at;
static int loads,flushes,reads,polls;
static jmp_buf stop;
static unsigned char disk_data[CS_REC_MAX];
static void mutex_lock(int*p){(void)p;}static void mutex_unlock(int*p){(void)p;}
static void queue_wait_w_tmo(int*q,struct queue_event*e,int timeout){(void)q;(void)timeout;if(polls++==2)longjmp(stop,1);current_tick+=100;e->id=0;e->data=0;}
static void queue_wait(int*q,struct queue_event*e){queue_wait_w_tmo(q,e,100);}
static bool queue_empty(int*q){(void)q;return true;}
static void usb_acknowledge(int a,int b){(void)a;(void)b;}
static bool tagcache_is_busy(void){return false;}
static bool disk_usb_pending(void){return false;}
static bool storage_disk_is_active(void){return false;}
void cs_unload(struct cider_store*s){s->loaded=false;}
int cs_load(struct cider_store*s,uint8_t*b,unsigned n,uint32_t seed){(void)b;(void)n;(void)seed;loads++;s->loaded=true;return 0;}
void cs_release(struct cider_store*s){(void)s;}
bool cs_maintain(struct cider_store*s,uint8_t*b,unsigned n,bool compact){(void)s;(void)b;(void)n;(void)compact;return false;}
bool cs_busy(const struct cider_store*s){(void)s;return false;}
static void snap_store(void){}
static bool read_store(const char*k,const char*t,bool tr,uint32_t req){(void)k;(void)t;(void)tr;(void)req;reads++;return true;}
static bool store_write_batch(void){flushes++;memset(&cache,0,sizeof cache);return false;}
static void build_list(void){}
static const char*tier_name(unsigned t){(void)t;return "M";}
'''
post=r'''
static void run(void){polls=0;if(!setjmp(stop))disk_worker();}
int main(void){load_requested=true;run();assert(loads==1&&store.loaded);puts("Cider-only cold boot creates/loads store without an already-spinning disk PASS");
memset(&cache,0,sizeof cache);cache.slot[0].length=32780;cache.slot[0].dirty=true;load_requested=false;flushes=0;dirty_at=0;run();assert(flushes>0&&diag.deferrals>0);puts("Dirty USB cover wakes and flushes after bounded idle deferral PASS");}
'''
(o/'test.c').write_text(pre+worker+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-I'+str(r/'apps-ipod'),str(o/'test.c'),'-o',str(o/'test')],check=True)
subprocess.run([str(o/'test')],check=True)
