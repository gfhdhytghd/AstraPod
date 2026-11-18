"""Production resident Cider demux/credit/CRC/cache; no USB/UI/disk."""
from podlink_rust_build import wire_args
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
s=(R/'apps-ipod/screens/playback/cider.c').read_text()
def fn(name):
 m=re.search(r'(?m)^static [\w *]+\b'+name+r'\([^;{}]*\)\s*\{',s);assert m,name
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
pre=r'''
#include <assert.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
static unsigned int play_request,loading_request;static struct cider_modes modes;static int play_reply;
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "screens/playback/cider_model.h"
#include "podlink.h"
#include "screens/playback/cider_store.h"
static struct cider_store_stats diag;
static bool host_diag=true,host_store,host_live,live_reply;
static long live_at;static char live_nonce[17];
static unsigned art_ack_request,art_ack_bytes;
static long dirty_at;
static int disk_queue;static bool disk_running=true;
static void queue_post(int*q,int v,int p){(void)q;(void)v;(void)p;}
static struct cider_state state;
static struct cider_transfer transfer;
static struct cider_cache cache;
static int cider_mutex;static unsigned long current_tick;
static unsigned int granted,grant,cancel_request,ncommands,art_revision;
#define HZ 100
#define BUTTON_REDRAW 1
static void button_queue_post(int b,int d){(void)b;(void)d;}
static void cider_library_reset(void){}
static void cider_library_invalidate(uint32_t n){(void)n;}
static bool cider_library_receive(const void*d,unsigned int n,uint32_t g,uint32_t l){(void)d;(void)n;(void)g;(void)l;return true;}
static bool disk_lookup,request_ready;
static unsigned int xs_count;
static uint32_t xs_request;
static char xs_keys[5][65];
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
static void mutex_lock(int*m){(void)m;}
static void mutex_unlock(int*m){(void)m;}
static void cider_log(const char*fmt,...){(void)fmt;}
static int pin_calls,store_texts;
static void pin_apply(void){pin_calls++;}
static void store_text(const unsigned char*d,unsigned int n){(void)d;(void)n;store_texts++;}
'''
post=r'''
static unsigned char art[32780],packet[12288];
static void put32(unsigned char*p,unsigned int v){for(int i=0;i<4;i++)p[i]=v>>(8*i);}
int main(void){
memcpy(packet,"CAR1",4);put32(packet+4,99);struct podlink_message message={.type=PODLINK_BINARY,.data=packet,.length=29};assert(!receive(&message));
const char*ca="CA\t7\t1\t";message=(struct podlink_message){.type=PODLINK_TEXT,.data=(const unsigned char*)ca,.length=strlen(ca)};assert(receive(&message)&&state.generation==7&&!host_diag);host_diag=true;
put32(art,8u<<16|0x5441);put32(art+4,128u<<16|128);put32(art+8,1);
transfer.active=true;transfer.generation=7;transfer.request=1;strcpy(transfer.tier,"M");memset(transfer.key,'a',64);transfer.key[64]=0;granted=CIDER_CREDIT;
for(unsigned int offset=0;offset<sizeof art;){unsigned int count=sizeof art-offset;if(count>12260)count=12260;memcpy(packet,"CAR1",4);put32(packet+4,7);put32(packet+8,1);put32(packet+12,offset);put32(packet+16,sizeof art);put32(packet+20,podlink_crc32(art,sizeof art));put32(packet+24,podlink_crc32(art+offset,count));memcpy(packet+28,art+offset,count);message=(struct podlink_message){.type=PODLINK_BINARY,.data=packet,.length=count+28};assert(receive(&message));assert(grant+granted==CIDER_CREDIT);offset+=count;}
assert(!transfer.active&&cider_cache_find(&cache,transfer.key,"M")>=0&&pin_calls>0);
assert(art_ack_request==1&&art_ack_bytes==sizeof art&&cache.slot[0].dirty);
unsigned int prior=granted;put32(packet+4,8);assert(receive(&message)&&granted==prior);
state.generation=0;put32(packet+4,7);assert(receive(&message));put32(packet+4,99);assert(!receive(&message));
message=(struct podlink_message){.type=PODLINK_TEXT,.data=(const unsigned char*)"echo",.length=4};assert(!receive(&message));
state.generation=7;play_request=123;
const char*ack="CK\t7\t123\t1\tOK";
message=(struct podlink_message){.type=PODLINK_TEXT,.data=(const unsigned char*)ack,.length=strlen(ack)};
assert(receive(&message)&&play_reply==1);
play_reply=0;ack="CK\t7\t124\t1\tOK";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&!play_reply);
ack="CK\t7\t123\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&play_reply==-1);
/* FU26: a failed shuffle/repeat command drops its shown-ahead value; success and unrelated requests do not */
cider_mode_ask(&modes,CIDER_MODE_SHUFFLE,1,10,200);cider_mode_ask(&modes,CIDER_MODE_REPEAT,2,10,201);
ack="CK\t7\t200\t1\tOK";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&modes.pending[0]&&modes.pending[1]);
ack="CK\t7\t999\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&modes.pending[0]&&modes.pending[1]);
ack="CK\t7\t201\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&modes.pending[0]&&!modes.pending[1]);
ack="CK\t7\t200\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&!modes.pending[0]);
const char*neighbors="CP\t7\tsong\t-\t-";strcpy(state.id,"song");message.data=(const unsigned char*)neighbors;message.length=strlen(neighbors);assert(receive(&message)&&!state.previous_art[0]&&!state.next_art[0]);
const char*challenge="CH\t7\t0123456789abcdef";message.data=(const unsigned char*)challenge;message.length=strlen(challenge);current_tick=1234;
assert(receive(&message)&&host_live&&live_reply&&live_at==1234&&!strcmp(live_nonce,"0123456789abcdef"));
challenge="CH\t8\t0000000000000000";message.data=(const unsigned char*)challenge;message.length=strlen(challenge);current_tick=1500;
assert(receive(&message)&&live_at==1234&&!strcmp(live_nonce,"0123456789abcdef"));
const char*offer="CO\t7\tM\taaaa";message.data=(const unsigned char*)offer;message.length=strlen(offer);assert(receive(&message)&&store_texts==1);
const char*feat="CF\t7\tlocalstore";message.data=(const unsigned char*)feat;message.length=strlen(feat);assert(receive(&message)&&store_texts==2);feat="CQ\t7\t0";message.data=(const unsigned char*)feat;message.length=strlen(feat);assert(receive(&message)&&store_texts==3);
/* FU28b: only the failed command owning the optimistic loading flag clears it. */
state.loading=true;loading_request=500;
ack="CK\t7\t499\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&state.loading);
ack="CK\t7\t500\t0\tFailed";message.data=(const unsigned char*)ack;message.length=strlen(ack);assert(receive(&message)&&!state.loading&&!loading_request);
puts("Production resident receiver: legacy raw passthrough, state negotiation, whole-cover CRC/cache, exact credit refund, stale-generation drop PASS");}
'''
(O/'receive.c').write_text(pre+'\nstatic volatile uint32_t status_word;\n'+fn('publish_status')+fn('u32')+fn('fail_art')+fn('finish_xs')+fn('receive')+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'apps-ipod'),'-I'+str(R/'firmware/export'),str(O/'receive.c'),str(R/'apps-ipod/screens/playback/cider_model.c'),*wire_args(),'-o',str(O/'receive')],check=True);subprocess.run([str(O/'receive')],check=True)
