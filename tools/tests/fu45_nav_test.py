"""FU45 production controller/root/WPS/carousel branches and body/remote maps.
Reuse FU43's deterministic drawing, audio and buffer adapters. No hardware I/O.
"""
from pathlib import Path
import re, subprocess, sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
adapter=R/'tools/tests/fu43_covers_nav_test.py'
ns={'__file__':str(adapter),'__name__':'fu45_adapters'}
exec(compile(adapter.read_text().split("subprocess.run(['cc'")[0],str(adapter),'exec'),ns)
source=ns['source'];fn=ns['fn']
source=source.replace('r->kind=CIDER_ROW_ALBUM;', "r->kind=CIDER_ROW_ALBUM;memset(r->art,'a',64);r->art[64]=0;")
keymap=(R/'apps-ipod/input/keymap_ipod.c').read_text()
carousel=(R/'apps-ipod/screens/covers/carousel.c').read_text()
rows=[]
for table in ['button_context_standard','remote_button_context_standard']:
 t=re.search(r'static const struct button_mapping '+table+r'\[\].*?\n};',keymap,re.S).group()
 rows+=re.findall(r'\{\s*ACTION_STD_(?:MENU|CANCEL|OK|QUICKSCREEN),[^}]+\}',t)
cover_rows=re.findall(r'\{PF_QUIT,[^}]+\}',carousel)
assert len(cover_rows)==2
extra=r'''
static int key_remote,nav_case,nav_step,album_visits,quick_visits;
struct key_entry{int action,button,previous;};
static const struct key_entry list_keys[]={LIST_KEYS};
static const struct key_entry cover_keys[]={COVER_KEYS};
static int key_action(const struct key_entry*map,unsigned n,int button,int previous){
 for(unsigned i=0;i<n;i++)if(map[i].button==button&&(!map[i].previous||map[i].previous==previous))return map[i].action;
 assert(0);return 0;
}
static int list_key(int body,int remote,int flag){
 int key=key_remote?remote:body;
 return key_action(list_keys,ARRAYLEN(list_keys),key|flag,flag?key:BUTTON_NONE);
}
static int menu_key(void){return list_key(BUTTON_MENU,BUTTON_RC_MENU,BUTTON_REL);}
static int cancel_key(void){return list_key(BUTTON_LEFT,BUTTON_RC_LEFT,0);}
static int ok_key(void){return list_key(BUTTON_SELECT,BUTTON_RC_SELECT,BUTTON_REL);}
static int cover_menu_key(void){int key=key_remote?BUTTON_RC_MENU:BUTTON_MENU;return key_action(cover_keys,ARRAYLEN(cover_keys),key|BUTTON_REL,key);}
'''.replace('LIST_KEYS',','.join(rows)).replace('COVER_KEYS',','.join(cover_rows))
# Insert before adapter functions so the real controller sees the key scripts.
pos=source.index('static bool list_do_action(int context')
source=source[:pos]+extra+source[pos:]
source=source.replace(fn(source,'list_do_action'),r'''
static bool list_do_action(int context,int timeout,struct gui_synclist*l,int*a){
 (void)context;(void)timeout;assert(nav_step<16);int s=nav_step++;
 if(nav_case==0){assert(s==0&&depth==0&&l->selected==0);*a=menu_key();return false;}
 if(nav_case==1){
  switch(s){
  case 0:assert(depth==0);l->selected=3;*a=ok_key();return false;
  case 1:assert(depth==1);l->selected=1;*a=ok_key();return false;
  case 2:assert(depth==2);l->selected=1;*a=list_key(BUTTON_MENU,BUTTON_RC_MENU,BUTTON_REPEAT);return false;
  case 3:assert(depth==2&&l->selected==1);*a=cancel_key();return false;
  case 4:assert(depth==1&&l->selected==1);*a=menu_key();return false;
  case 5:assert(depth==0&&l->selected==3);*a=menu_key();return false;
  }assert(0);
 }
 if(nav_case==2){
  if(s==0){l->selected=3;*a=ok_key();return false;}
  if(s==1){assert(depth==1);l->selected=2;*a=ok_key();return false;}
  assert(s==2&&depth==2);*a=ACTION_REMOTE_WPS;return false;
 }
 if(nav_case==3){assert(s==0&&depth==1);*a=cancel_key();return false;}
 if(nav_case==4){if(s==0)assert(depth==2&&l->selected==2);*a=menu_key();return false;}
 assert(0);
}
''')
quick=r'''
static int quick_screen_current(int action,void(*poll)(void)){
 (void)poll;assert(action==ACTION_STD_QUICKSCREEN&&depth==2&&history[depth].selected==1);quick_visits++;return 0;
}
'''
source=source.replace(fn(source,'quick_screen_current'),'static int quick_screen_current(int,void(*)(void));')
source=source.replace(fn(source,'cider_covers'),r'''
int cider_covers(void){
 assert(active&&available&&!claimed);assert(agents_nav_enter(AGENTS_NAV_COVERS));claimed=true;covers_calls++;
 int result=nav_case==3&&album_visits++==0 ? GO_TO_WPS : cover_event(cover_menu_key());claimed=false;agents_nav_leave(AGENTS_NAV_COVERS);return result;
}
''')
source=source.replace(fn(source,'album_covers_return'),r'''
static int album_covers_return(const char*file,bool*toggle){
 (void)file;assert(!active&&!claimed);assert(agents_nav_enter(AGENTS_NAV_COVERS));claimed=true;covers_calls++;
 int result=cover_event(cover_menu_key());if(toggle)*toggle=false;claimed=false;agents_nav_leave(AGENTS_NAV_COVERS);return result;
}
''')
source=source.replace(fn(source,'main'),quick+r'''
static void fresh(int online,int test){reset(online);nav_case=test;nav_step=album_visits=quick_visits=0;}
static void released(void){assert(!claimed&&!agents_nav_active()&&!cover_nav_active()&&activity_count==0);}
int main(void){
 for(int i=0;i<=GO_TO_CIDER;i++)items[i]=(struct root_item){generic,(void*)(intptr_t)i};
 items[GO_TO_CIDER]=(struct root_item){cider_scrn,0};
 for(key_remote=0;key_remote<2;key_remote++){
  /* A local cover peek returns through the WPS parent on MENU. */
  fresh(0,0);long go=999;
  assert(show_album_covers(&go,false)&&go==GO_TO_PREVIOUS&&covers_calls==1);released();
  /* Native/Cider WPS -> covers -> MENU leaves the visit. */
  fresh(1,0);cider_browse_covers(true);
  assert(load_screen(GO_TO_CIDER)==GO_TO_PREVIOUS&&last_screen==GO_TO_ROOT&&nav_step==0);released();
  /* The carousel-owned album panel returns through the same cover visit. */
  fresh(1,3);cider_browse_covers(true);assert(load_screen(GO_TO_CIDER)==GO_TO_WPS);
  assert(cover_nav_playing()&&cover_suspended&&!claimed&&!agents_nav_active());
  cider_browse_covers(false);assert(load_screen(GO_TO_CIDER)==GO_TO_ROOT);
  assert(covers_calls==2&&nav_step==0&&last_screen==GO_TO_CIDER);released();
  /* Other entry points retain their ordinary menu/browser parent. */
  fresh(0,0);cover_nav_begin(GO_TO_DBBROWSER);assert(album_covers_scrn(NULL)==GO_TO_DBBROWSER);released();
  fresh(1,4);depth=2;history[2].selected=2;cover_nav_begin(GO_TO_CIDER);cider_browse_covers(false);
  assert(cider_browse()==GO_TO_ROOT&&nav_step==3);released();
  /* A child and QuickScreen keep selections during this visit. */
  fresh(1,1);assert(cider_browse()==GO_TO_ROOT&&nav_step==6&&quick_visits==1);released();
  /* Leave from a child, re-enter with no test reset: first frame is root. */
  fresh(1,2);assert(cider_browse()==GO_TO_WPS&&nav_step==3);released();
  nav_case=0;nav_step=0;assert(cider_browse()==GO_TO_ROOT&&nav_step==1);released();
  nav_step=0;assert(cider_browse()==GO_TO_ROOT&&nav_step==1);released();
 }
 puts("FU45 body + remote: local/Cider WPS cover MENU, album child return, other parents, child/QuickScreen positions, fresh/repeated entry root PASS");
}
''')
(O/'fu45.c').write_text(source)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-enum-conversion','-Wno-unused-function','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),str(O/'fu45.c'),'-o',str(O/'fu45')],check=True)
subprocess.run([str(O/'fu45')],check=True,timeout=30)
