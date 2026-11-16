"""FU35 Home triple-Play through real gesture/router/menu; no device access.
Uses FU32's deterministic drawing/settings adapters. Production get_action's
pending-nav branch is retained; button queue/time are modeled. The baseline
fails by exhausting a 1000-iteration budget, rather than waiting for HANG.
"""
from pathlib import Path
import re, runpy, subprocess, sys
R=Path(__file__).resolve().parents[2]; O=Path(sys.argv[1]).resolve(); O.mkdir(parents=True,exist_ok=True)
ns=runpy.run_path(str(R/'tools/tests/fu32_menu_path_test.py'),run_name='fu35_adapters')
fn=ns['fn']; pre=ns['pre']; loop=ns['loop']; controls=ns['controls']
a=(R/'apps-ipod/input/action.c').read_text(); root=(R/'apps-ipod/root_menu.c').read_text()
route=fn(a,'route_screen_shortcut'); place=fn(root,'root_menu_place_cider')
pre=pre.replace('static const struct menu_item_ex root_menu_={0};','static struct menu_item_ex root_menu_;')
pre=pre.replace('static bool root_menu_refresh(void){return false;}','static bool root_menu_refresh(void);')
pre=pre.replace(fn(pre,'get_action'), '')
extra=set(re.findall(r'\b(?:ACTION_|GO_TO_|CONTEXT_|AGENTS_NAV_)\w+',route+loop+place)) - set(ns['tokens'])
extra|={'AGENTS_NAV_COVERS','AGENTS_NAV_NONE','AGENTS_NAV_BACK','AGENTS_NAV_WPS','AGENTS_NAV_AGENTS','CONTEXT_AGENTS','CONTEXT_WPS'}
extra-=set(ns['tokens'])
pre=pre.replace('struct viewport{','enum agents_nav_screen {'+','.join(sorted(extra))+'};\nstruct viewport{',1)
# Use the real root reorder, including its bounded memcmp, on each menu turn.
pre+=r'''
#define HAVE_USB_PODLINK
#define BUTTON_REDRAW 0x20000000
#define MENU_COUNT_MASK 0xffff
#define MENU_COUNT_SHIFT 8
#define MENU_ITEM_COUNT(n) ((n)<<8)
#define MAX_MENU_ITEMS 8
static struct menu_item_ex *root_menu__[MAX_MENU_ITEMS];
static struct menu_item_ex cider_item, shortcut_menu, row_item;
static bool online=true,remote_input_active;
static int pending, polls, refreshes, source_calls, toggle_pending, fill_pending;
static int agents_nav_pending(void){return pending;}
static bool agents_nav_has(int n){(void)n;return false;}
static void agents_nav_request(int n,bool remote){(void)remote;pending=n;}
static bool shortcut_screen_context(int c){return c==CONTEXT_WPS||c==CONTEXT_AGENTS;}
static enum agents_nav_screen agents_cover_toggle_target(void){return AGENTS_NAV_COVERS;}
static void cider_remote_source(void){source_calls++;}
static void root_menu_agents_shortcut(void){}
static void root_menu_wps_shortcut(void){}
static bool cider_available(void){return online;}
static int item_callback(int a,const struct menu_item_ex*m,struct gui_synclist*l){(void)m;(void)l;return a;}
static int shortcut_menu_callback(int a,const struct menu_item_ex*m,struct gui_synclist*l){return item_callback(a,m,l);}
#define REMOTE_LOG_DISABLE
#include "remote_log.h"
#include "input/agents_remote.h"
static struct agents_remote_state remote_state;
'''
remote=re.sub(r'^#include .*$', '', (R/'apps-ipod/input/agents_remote.c').read_text(),flags=re.M)
pre+=remote+place+route
pre+=r'''
static bool root_menu_refresh(void){assert(++refreshes<=1000);return root_menu_place_cider();}
static int get_action_worker(action_last_t*l,action_cur_t*c){
 (void)l;(void)c;
 assert(++polls<=1000); /* old Home loop never blocks once pending is set */
 static const int buttons[]={BUTTON_RC_PLAY,BUTTON_RC_PLAY|BUTTON_REL,BUTTON_RC_PLAY,BUTTON_RC_PLAY|BUTTON_REL,BUTTON_RC_PLAY,BUTTON_RC_PLAY|BUTTON_REL};
 static const unsigned times[]={100,113,128,141,156,169};
 if(polls>6)return ACTION_NONE;
 int b=buttons[polls-1];
 int out=agents_remote_button(&remote_state,REMOTE_OTHER,b,b&BUTTON_REL?0:BUTTON_RC_PLAY,times[polls-1]);
 if(out==REMOTE_COVERS_BUTTON)return ACTION_AGENTS_REMOTE_COVERS;
 if(out==REMOTE_SWITCH_BUTTON)return ACTION_REMOTE_WPS;
 if(out==BUTTON_RC_PLAY)toggle_pending++;
 return ACTION_NONE;
}
static action_last_t action_last;
static void podlink_time_sync(void){}
static void init_act_cur(action_cur_t*c,int ctx,int t,void*p){(void)c;(void)ctx;(void)t;(void)p;}
static int do_backlight(action_last_t*l,action_cur_t*c,int a){(void)l;(void)c;return a;}
'''
pre+=fn(a,'get_action')
post=r'''
int main(void){
 for(unsigned i=0;i<ARRAYLEN(labels);i++)snprintf(labels[i],sizeof labels[i],"label-%u",i);
 for(int slow=0;slow<2;slow++){
  remote_state=(struct agents_remote_state){0};pending=AGENTS_NAV_NONE;polls=refreshes=source_calls=0;
  toggle_pending=slow;fill_pending=1;
  root_menu__[0]=&row_item;root_menu__[1]=&cider_item;
  root_menu_.flags=MT_MENU|MENU_ITEM_COUNT(2);root_menu_.submenus=(const struct menu_item_ex**)root_menu__;
  int selected=0;assert(do_menu(&root_menu_,&selected,NULL,false)==GO_TO_PREVIOUS);
  assert(polls==6&&pending==AGENTS_NAV_COVERS&&source_calls==1);
  assert(toggle_pending==slow&&fill_pending==1&&refreshes<=6);
  /* The root dispatcher consumes exactly this request after menu teardown. */
  pending=AGENTS_NAV_NONE;
 }
 puts("FU35 Home triple Play: gesture -> router -> production get_action/do_menu/root reorder; pending toggle+unanswered fill; <=6 polls PASS");
}
'''
source=pre+fn((R/'apps-ipod/widgets/list.c').read_text(),'simplelist_show_list')+loop+controls+post
(O/'home.c').write_text(source)
cmd=['cc','-std=gnu11','-Wno-enum-conversion','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),'-I'+str(R/'firmware/export')]
subprocess.run(cmd+[str(O/'home.c'),'-o',str(O/'home')],check=True)
subprocess.run([str(O/'home')],check=True,timeout=10)
# Counterfactual: remove only the fix from the same production loop.
old=re.sub(r'#ifdef HAVE_USB_PODLINK\n        /\* Root CANCEL.*?#endif\n','',source,flags=re.S)
assert old!=source
(O/'home-old.c').write_text(old)
subprocess.run(cmd+[str(O/'home-old.c'),'-o',str(O/'home-old')],check=True)
r=subprocess.run([str(O/'home-old')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=10)
(O/'baseline.log').write_text(r.stdout)
assert r.returncode!=0 and 'refreshes' in r.stdout # pending path bypasses the button worker
