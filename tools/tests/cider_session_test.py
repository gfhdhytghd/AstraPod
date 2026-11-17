#!/usr/bin/env python3
"""FU31 production challenge parser, disconnect labels and 4-second device expiry."""
from podlink_rust_build import wire_args
from pathlib import Path
import re, subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
s=(R/'apps-ipod/screens/playback/cider.c').read_text()
def fn(name):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',s);assert m,name
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "screens/playback/cider_model.h"
#define HZ 100
static bool initialized=true,host_live=true,wire=true;
static long current_tick=1000,live_at=1000;
static struct cider_state state={.generation=7};
static bool podlink_host_connected(void){return wire;}
'''+fn('cider_connected')+r'''
int main(void){char nonce[17]="unchanged";
 const char *ok="CH\t7\t0123456789abcdef";
 assert(cider_live_parse(ok,strlen(ok),7,nonce)&&!strcmp(nonce,"0123456789abcdef"));
 const char *bad[]={"CH\t8\t0123456789abcdef","CH\t7\t0123456789abcdeF","CH\t7\t0123456789abcde","CH\t7\t0123456789abcdefX","CH\t4294967296\t0123456789abcdef","CH\t\t0123456789abcdef","CH\t7\t0123456789abcdeg","CH\t7\t0123456789abcdef\t"};
 for(unsigned i=0;i<sizeof bad/sizeof*bad;i++)assert(!cider_live_parse(bad[i],strlen(bad[i]),7,nonce));
 assert(!cider_live_parse(ok,strlen(ok),0,nonce));
 char embedded[48];memcpy(embedded,ok,strlen(ok));embedded[6]=0;
 assert(!cider_live_parse(embedded,strlen(ok),7,nonce));
 assert(cider_connected());current_tick=1399;assert(cider_connected());
 current_tick=1400;assert(!cider_connected()); /* CN cannot refresh live_at */
 live_at=current_tick;assert(cider_connected());wire=false;assert(!cider_connected());
 wire=true;state.generation=0;assert(!cider_connected());state.generation=7;
 host_live=false;current_tick=10000;assert(cider_connected()); /* old host */
 assert(!strcmp(cider_list_notice(false,false,0,false),"%s not connected"));
 assert(!strcmp(cider_list_notice(true,false,0,false),"%s unavailable"));
 assert(!strcmp(cider_list_notice(true,true,-1,false),"Loading..."));
 assert(!strcmp(cider_list_notice(true,true,0,true),"%s list unavailable"));
 assert(!strcmp(cider_list_notice(true,true,0,false),""));
 assert(!cider_list_notice(true,true,1200,false));
 puts("FU31 challenge validation, device deadline, legacy idle and disconnected/loading/failed/empty labels PASS");}
'''
(O/'session.c').write_text(code)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R/'apps-ipod'),'-I'+str(R/'firmware/export'),str(O/'session.c'),str(R/'apps-ipod/screens/playback/cider_model.c'),*wire_args(),'-o',str(O/'session')],check=True)
subprocess.run([str(O/'session')],check=True)
# Scheduling and no-credit path in the real resident service.
assert fn('next').index('else if (live_reply)') < fn('next').index('else if (host_diag && art_ack_request)')
assert fn('next').index('reset();') < fn('next').index('mutex_lock(&cider_mutex)')
assert 'cider_live_parse' in fn('receive') and 'localdiag-live' in fn('next')
b=(R/'apps-ipod/screens/playback/cider_browser.c').read_text();assert 'list_notice = cider_list_notice(cider_connected()' in b
print('Production priority reply, pre-lock expiry/reset, negotiation and browser wiring PASS')
