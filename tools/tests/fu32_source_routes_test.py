"""FU32 remote double/triple source preference through production entry points."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
def fn(s,n):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
r=(R/'apps-ipod/root_menu.c').read_text();c=(R/'apps-ipod/screens/playback/cider.c').read_text();functions=fn(r,'root_menu_wps_shortcut')+fn(r,'root_menu_cover_shortcut')+fn(r,'album_covers_scrn')
a=(R/'apps-ipod/input/action.c').read_text();route=fn(a,'route_screen_shortcut')
tokens=sorted(set(re.findall(r'\b(?:ACTION_|GO_TO_|AGENTS_NAV_)\w+',functions+route)))
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define HAVE_USB_PODLINK
#define AUDIO_STATUS_PLAY 1
static bool active,online,paused;static int pending;
static int target,local_calls,cider_calls;
static bool wps_return_agents,wps_menu_covers;
static bool cider_available(void){return online;}static bool cider_active(void){return active;}
static int audio_status(void){return paused?0:1;}static void audio_pause(void){paused=true;}
static int agents_nav_pending(void){return pending;}
static bool agents_nav_has(int n){(void)n;return false;}static void agents_nav_request(int n,bool b){(void)b;pending=n;}
static void run_screen_shortcut(int s){target=s;}
static bool cover_nav_playing(void){return true;}static bool cover_nav_active(void){return false;}static void cover_nav_begin(int n){(void)n;}static int cover_nav_finish(void){return GO_TO_PREVIOUS;}
static bool wait_for_album_tables(void){return true;}static void album_covers_cancel_resume(void){}
static int album_covers_return(void*p,void*q){(void)p;(void)q;local_calls++;return GO_TO_PREVIOUS;}
static int cider_browse_covers(bool b){(void)b;cider_calls++;return GO_TO_CIDER;}
'''
pre='enum agents_nav_screen {'+','.join(sorted(set(tokens)|{'GO_TO_CIDER'}))+'};\n'+pre
pre+='\nstatic bool shortcut_screen_context(int context){return context==1;}static void root_menu_agents_shortcut(void){}\n'
pre+=fn(c,'cider_remote_source')+functions+route
post=r'''
int main(void){for(int available=0;available<2;available++){
 online=available;active=!online;paused=false;pending=AGENTS_NAV_NONE;root_menu_wps_shortcut();assert(target==GO_TO_WPS&&active==online);assert(paused==(bool)available);
 active=!online;paused=false;root_menu_cover_shortcut();assert(target==GO_TO_ALBUM_COVERS&&active==online);
 int before=local_calls;int next=cider_calls;int result=album_covers_scrn(NULL);
 assert(result==(online?GO_TO_CIDER:GO_TO_PREVIOUS));assert(local_calls==before+!online&&cider_calls==next+online);
 for(int context=0;context<2;context++){
  pending=AGENTS_NAV_NONE;active=!online;paused=false;
  int routed=route_screen_shortcut(ACTION_AGENTS_REMOTE_COVERS,context);
  assert(active==online);assert(context==1?routed==ACTION_AGENTS_REMOTE_COVERS:pending==AGENTS_NAV_COVERS);
  pending=AGENTS_NAV_NONE;active=!online;paused=false;
  route_screen_shortcut(ACTION_REMOTE_WPS,context);assert(active==online);
 }
 }puts("FU32 double WPS/triple Covers source preference: available Cider, unavailable local; pause policy and actual Covers entry PASS");}
'''
(O/'test.c').write_text(pre+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
