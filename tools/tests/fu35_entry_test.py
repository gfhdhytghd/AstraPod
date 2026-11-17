"""Production Cider entry/page recovery with store-present deterministic timing.
The carousel/display adapter records its first-frame entry; no LCD or USB.
"""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
def fn(s,n):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "screens/playback/cider_library.h"
#include "screens/playback/cider_store.h"
#define HZ 100
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define CONTEXT_STD 0
#define ACTION_STD_CANCEL 1
#define ACTION_STD_MENU 4
#define ACTION_STD_QUICKSCREEN 2
#define QUICKSCREEN_IN_USB 3
#define GO_TO_ROOT -2
static int quick_calls;
static int quick_screen_current(int a,void*p){(void)a;(void)p;quick_calls++;return 0;}
#define AGENTS_NAV_NONE 0
#define GO_TO_PREVIOUS -1
#define LANG_CAROUSEL_NO_ALBUMS "none"
#define ID2P(x) (x)
#define CAROUSEL_REMOTE_RETURN 2
long current_tick;
static int generation=7,mode,polls,first_tick,cancels,store_reads;
static unsigned int revision;
static const int model;
static struct cider_store store;
static void cider_page_open(const char*q){cider_library_open(q);}
static int cider_page_total(void){return cider_library_total();}
static int agents_nav_pending(void){return 0;}
static unsigned int cider_art_revision(void){return 0;}
static void splash(int t,const char*s){(void)t;(void)s;}
static void cider_cancel_art(void){cancels++;}
static int carousel_run(const void*m,void*p){(void)m;(void)p;assert(store.loaded);assert(store_reads==0);first_tick=current_tick;return 0;}
static int get_action(int c,int timeout){
 (void)c;assert(timeout>0);polls++;current_tick+=timeout;
 if(mode==1)return ACTION_STD_CANCEL;
 if(mode==2)return 0; /* no host answer */
 char req[512],wire[400];unsigned g,id,offset;
 assert(cider_library_next(req,sizeof req,generation));
 assert(sscanf(req,"cq\t%u\t%u\t%u",&g,&id,&offset)==3);
 int n=snprintf(wire,sizeof wire,"CL\t%u\t%u\t1\t0\t1\t0\t1\nA\tAlbum\tArtist\t-\t0\t0\talbum\talbum/A\t181818\t2020\n",g,id);
 assert(cider_library_receive(wire,n,g,1));return 0;
}
'''
post=r'''
int main(void){
 store.loaded=true;mode=0;
 for(int session=0;session<3;session++){
  generation++;cider_library_reset();polls=0;first_tick=-1;long start=current_tick;
  cider_covers();assert(polls==1&&first_tick-start==10&&cider_page_total()==1);
 }
 /* A warm page enters immediately, with no disk scan on the UI path. */
 long start=current_tick;polls=0;cider_covers();assert(!polls&&first_tick==start);
 mode=2;cider_library_reset();polls=0;start=current_tick;cider_covers();assert(polls==50&&current_tick-start==5*HZ);
 mode=1;cider_library_reset();polls=0;cider_covers();assert(polls==1);
 /* Malformed counts/overflow cannot manufacture rows outside the page. */
 char request[512],wire[512];unsigned gen,id,off;
 cider_library_reset();assert(cider_library_next(request,sizeof request,generation));sscanf(request,"cq\t%u\t%u\t%u",&gen,&id,&off);
 int n=snprintf(wire,sizeof wire,"CL\t%u\t%u\t1\t0\t0\t0\t1\n",gen,id);assert(!cider_library_receive(wire,n,gen,1));
 n=snprintf(wire,sizeof wire,"CL\t%u\t4294967296\t1\t0\t1\t0\t0\n",gen);assert(!cider_library_receive(wire,n,gen,1));
 n=snprintf(wire,sizeof wire,"CL\t%u\t%u\t1\t0\t1\t2\t0\n",gen,id);assert(!cider_library_receive(wire,n,gen,1));
 puts("FU35 entry/store present: first-frame adapter 100ms after one modeled page wait; warm page 0ms; 3 generations recover; absent page exits at 5000ms; cancel 100ms PASS");
}
'''
(O/'config.h').write_text('');(O/'kernel.h').write_text('#define HZ 100\n#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)\nextern long current_tick;\n')
(O/'entry.c').write_text(pre+fn((R/'apps-ipod/screens/covers/cider_covers.c').read_text(),'cider_covers')+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R/'apps-ipod'),str(O/'entry.c'),str(R/'apps-ipod/screens/playback/cider_library.c'),'-o',str(O/'entry')],check=True)
subprocess.run([str(O/'entry')],check=True)
