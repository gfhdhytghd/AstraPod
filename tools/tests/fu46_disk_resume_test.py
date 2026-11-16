"""Production resident reset/reconnect plus a burst of failed XS fills.

The disk/session boundary is modeled; this does not boot or emulate the UI,
USB controller or scheduler. The existing USB/store test covers unload/reload.
"""
from pathlib import Path
import runpy
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve()
O.mkdir(parents=True, exist_ok=True)
ns = runpy.run_path(str(R / 'tools/tests/cider_pin_test.py'), run_name='fu46_pin')
fn = ns['fn']
source = (O / 'pin.c').read_text().split('int main(void){', 1)[0]
source = source.replace('static void reset(void){host_live=live_reply=false;state.generation=0;}',
                        'static void reset(void);')
source += r'''
#include "podlink.h"
static uint32_t status_word, loading_request, play_request;
static struct cider_modes modes;
static int play_reply;
#define BUTTON_REDRAW 1
static void button_queue_post(int b,int d){(void)b;(void)d;}
static void cider_library_reset(void){}
static void cider_library_invalidate(uint32_t g){(void)g;}
static bool cider_library_receive(const void*d,unsigned n,uint32_t g,uint32_t l)
{(void)d;(void)n;(void)g;(void)l;return false;}
'''
source += ''.join(fn(n) for n in ['publish_status', 'reset', 'u32', 'receive'])
source += r'''
static void text(const char*s){
 struct podlink_message m={.type=PODLINK_TEXT,.data=(const unsigned char*)s,.length=strlen(s)};
 assert(receive(&m));
}
static void reconnect(void){
 /* USB service invalidates the resident on detach before the new HR/CA.
  * Worker unload/remount order is exercised in cider_usb_test.py. */
 reset();assert(!state.generation&&!transfer.active&&!noffers&&!nacks&&!host_store);
 store.loaded=false;store.loaded=true;
 text("CA\t7\t1\t");assert(state.generation==7);
 text("CF\t7\tlocalstore");text("CF\t7\tlocaldiag");
 assert(host_store&&host_diag&&load_requested);
}
int main(void){
 key(A,'a');key(B,'b');key(C,'c');
 store.cap=CS_DEFAULT_CAP;store.evict=store.compact=store.cur=-1;
 for(int session=0;session<8;session++){
  reconnect();pin_on=false;noffers=ncommands=0;current_tick+=10*HZ;
  art_used_at=current_tick-4*HZ;want_ready=ls_due=false;diag_at=current_tick;
  grant=0;granted=CIDER_CREDIT;cancel_request=0;cancel_all=false;
  for(int request=26;request<35;request++){
   char offer[256],reply[192],failed[65];key(failed,'a');failed[0]='0'+(request-26);
   snprintf(offer,sizeof offer,"CO\t7\tXS\t%s",failed);text(offer);
   current_tick+=8;assert(next(out,sizeof out));
   /* Periodic inventory/diagnostics may precede the art request. */
   for(int i=0;i<8&&strncmp(out,"cr\t",3);i++)next(out,sizeof out);
   assert(!strncmp(out,"cr\t",3)&&transfer.active&&transfer.fill);
   uint32_t id=transfer.request;
   snprintf(reply,sizeof reply,"CK\t7\t%u\t0\tArtwork URL has no capped size variant",id);
   text(reply);assert(!transfer.active&&!disk_lookup&&!request_ready&&!xs_count);
   assert(cider_cache_backoff(&cache,failed,"XS",current_tick));
  }
  /* The next single XS succeeds after nine consecutive failures. */
  char offer[256];snprintf(offer,sizeof offer,"CO\t7\tXS\t%s",C);text(offer);
  /* Model the host pause between offer bursts. */
  current_tick+=6*HZ;next(out,sizeof out);
  for(int i=0;i<8&&strncmp(out,"cr\t",3);i++)next(out,sizeof out);
  assert(transfer.active&&!strncmp(out,"cr\t",3));
  static unsigned char packet[2088];memset(packet,0,sizeof packet);
  memcpy(packet,"CAR1",4);
  uint32_t fields[]={7,transfer.request,0,2060,0,0};
  packet[28]=0x41;packet[29]=0x54;packet[30]=8;packet[32]=32;packet[34]=32;
  fields[4]=fields[5]=podlink_crc32(packet+28,2060);
  for(int i=0;i<6;i++)for(int j=0;j<4;j++)packet[4+4*i+j]=fields[i]>>(8*j);
  struct podlink_message m={.type=PODLINK_BINARY,.data=packet,.length=sizeof packet};
  assert(receive(&m)&&!transfer.active&&art_ack_request==fields[1]);
  assert(cider_cache_find(&cache,C,"XS")>=0);
  assert(next(out,sizeof out)&&!strncmp(out,"lr\t",3));
  /* Flush is modeled before the next detach; no dirty fill gate leaks. */
  memset(&cache,0,sizeof cache);
 }
 puts("FU46 resident: 8 detach/reset/reconnect sessions; CF/CO; 9 terminal failures then XS success/ACK per session PASS");
}
'''
(O / 'resume.c').write_text(source)
cmd = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Wno-unused-variable',
       '-Wno-unused-function', '-Wno-unused-parameter', '-Werror',
       '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
       '-I' + str(R / 'apps-ipod'), '-I' + str(R / 'firmware/export'),
       str(O / 'resume.c'), str(R / 'apps-ipod/screens/playback/cider_model.c'),
       str(R / 'apps-ipod/screens/playback/cider_store.c'),
       str(R / 'firmware/usbstack/podlink_wire.c'), '-o', str(O / 'resume')]
subprocess.run(cmd, check=True)
subprocess.run([str(O / 'resume')], check=True, timeout=10)
