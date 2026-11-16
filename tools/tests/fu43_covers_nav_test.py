"""FU43: real root dispatch/Cider loop/gestures with bounded screen adapters."""
from pathlib import Path
import re, subprocess, sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
old=len(sys.argv)>2
B=Path(sys.argv[2]).resolve() if old else R/'apps-ipod'
def read(n):return (B/n).read_text()
def fn(s,n):
 m=re.search(r'(?m)^(?:static[ \t]+)?(?:inline[ \t]+)?(?:int|bool|void|long|enum agents_nav_screen)[ \t*]+'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
root=read('root_menu.c');cider=read('screens/playback/cider_browser.c');action=read('input/action.c');carousel=read('screens/covers/carousel.c');wps=read('screens/playback/wps.c')
# Reuse the deterministic widget/provider adapters; execute only their setup.
adapter=R/'tools/tests/cider_browser_test.py';namespace={'__file__':str(adapter),'__name__':'fu43_adapters'}
exec(compile(adapter.read_text().split("post=r'''")[0],str(adapter),'exec'),namespace)
pre=namespace['pre']
for n in ['cover_nav_active','cover_nav_begin','cover_nav_finish','cover_nav_origin_is','agents_nav_enter','agents_nav_leave','agents_nav_take','agents_nav_pending','cider_active','cider_remote_source','audio_status','list_do_action','push_current_activity','pop_current_activity','cider_play','quick_screen_current']:
 pre=pre.replace(fn(pre,n),'')
pre=re.sub(r'enum \{ACTION_NONE,.*?\};','',pre,flags=re.S)
pre=re.sub(r'enum agents_nav_screen \{.*?\};','',pre,flags=re.S)
root_functions='\n'.join(fn(root,n) for n in ['artist_portraits_scrn','wpsscrn','cover_tracks_scrn','album_covers_scrn','cider_scrn','agents_scrn','load_screen','wps_back_to'])
route=fn(action,'shortcut_screen_context')+fn(action,'route_screen_shortcut')
nav=read('input/agents_shortcut.c');nav=nav[nav.index('static bool cover_journey'):nav.index('int agents_shortcut_button(')]
if 'agents_cover_toggle_target(' not in nav:nav+='\nenum agents_nav_screen agents_cover_toggle_target(void){return agents_nav_has(AGENTS_NAV_COVERS)?AGENTS_NAV_BACK:AGENTS_NAV_COVERS;}\n'
chord=fn(read('input/agents_shortcut.c'),'agents_cover_chord_button')
a=carousel.index('        enum agents_nav_screen nav =',carousel.index('static int album_covers_loop(void)'));z=carousel.index('        case PF_WPS:',a)
cover_case=carousel[a:z]+'        default:break;\n        }\n return 9999;\n'
a=root.index('            if (cover_nav_active())',root.index('        case GO_TO_ALBUM_COVERS_TRACKS:',root.index('        case GO_TO_ALBUM_COVERS_TRACKS:')+1));z=root.index('            if (ret_val == GO_TO_ROOT)',a);track_case=root[a:z].replace('                break;','                return ret_val;')
header=(R/'apps-ipod/root_menu.h').read_text();a=header.index('enum {');root_enum=header[a:header.index('};',a)+2]
extra=set(re.findall(r'\b(?:ACTION_|CONTEXT_|ACTIVITY_|LANG_|Icon_)[\w]+',cider+root_functions+route+cover_case+fn(wps,'show_album_covers')))
extra.update({'CONTEXT_STD','CONTEXT_QUICKSCREEN','ACTIVITY_MAINMENU','ACTIVITY_AGENTS'})
extra.discard('ACTION_NONE')
prefix='''#define HAVE_USB_PODLINK
#define IPOD_ACCESSORY_PROTOCOL
#define REMOTE_LOG_DISABLE
#include "firmware/target/arm/ipod/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#define SYS_EVENT 0x80000000u
#define BUTTON_MULTIMEDIA 0x10000000
#include "firmware/export/remote_log.h"
#include "apps-ipod/input/agents_shortcut.h"
#define TAGNAVI_MAIN_MENU_SLOTS 20
'''+root_enum+'\nenum current_activity {ACTION_NONE=0,'+','.join(sorted(extra))+'};\n'
pre=pre.replace('#define SYS_USB_CONNECTED 0xffff','#define SYS_USB_CONNECTED 0xffff')
pre=pre.replace('static struct {bool db_albumart;int db_art_row_height;}global_settings','static struct {bool db_albumart;int db_art_row_height;int wps_menu_action;}global_settings')
pre=pre.replace('static bool cider_play_in(const char*p,const char*c,int i)', 'static bool cider_play_in(const char*p,const char*c,int i)')
# Forward declarations used by the unchanged source adapters.
pre=pre.replace('global_settings={true,46};','global_settings={true,46,0};')
pre=pre.replace('static char last_ctx[400];','static bool cider_play(const char*);\nstatic char last_ctx[400];')
setup=r'''
#define AUDIO_STATUS_PLAY 1
#define AUDIO_STATUS_PAUSE 2
#define WPS_MENU_PREVIOUS 1
#define PLAYLIST_CONTROL_FILE "control"
#define ID2P(x) str(x)
#define DEBUGF(...)
#define PF_QUIT ACTION_STD_MENU
#define PF_BACK ACTION_STD_CANCEL
#define AGENTS_SWITCH_WPS 2
#define AGENTS_SWITCH_COVERS 3
#define AGENTS_SWITCH_REMOTE_WPS 4
#define CAROUSEL_REMOTE_RETURN -1001
static int activity_stack[16],activity_count,activity=ACTIVITY_MAINMENU;
static int last_screen=GO_TO_ROOT,album_tracks_from=GO_TO_ROOT;
static bool cover_toggled_back,back_from_wps,came_back,wps_return_agents,wps_menu_covers;
static struct {int last_screen,resume_index,resume_crc32,resume_offset,resume_elapsed;}global_status;
static bool active,claimed,paused,remote_input_active,use_quick;static int normal_depth=2,menus,stage,gesture,expected,returned,covers_calls,source_calls;
static int script,first_browser_depth=-1,opened_album,overlay_open,overlay_closes;
static int agent_view=41,wps_view=73,expected_agent_view,expected_wps_view;
static int action_budget;
static int route_screen_shortcut(int,int);
static int album_covers_scrn(void*);static int cider_scrn(void*);static int cover_tracks_scrn(void*);
static int agents_screen(void);static long gui_wps_show(bool,bool);
static int album_covers_return(const char*,bool*);
static void push_current_activity(int n){assert(activity_count<16);activity_stack[activity_count++]=activity;activity=n;}
static void pop_current_activity(void){assert(activity_count);activity=activity_stack[--activity_count];}
static void pop_current_activity_without_refresh(void){pop_current_activity();}
static int get_current_activity(void){return activity;}
static bool app_buffer_is_claimed(void){return claimed;}
static bool cider_active(void){return active;}
static int audio_status(void){return AUDIO_STATUS_PLAY|(paused?AUDIO_STATUS_PAUSE:0);}
static void audio_pause(void){paused=true;}
static void cider_remote_source(void){source_calls++;if(available)audio_pause();active=available;}
static bool cider_play(const char*p){(void)p;plays++;return true;}
static bool wait_for_album_tables(void){return true;}
static void album_covers_cancel_resume(void){}
static bool album_covers_reopen_tracks(void){return true;}
static void gwps_leave_wps(bool b){(void)b;}
static void status_save(bool b){(void)b;}
static void talk_shutup(void){}
static int playlist_resume(void){return 0;}
static void playlist_resume_track(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
static void playlist_start(int a,int b,int c){(void)a;(void)b;(void)c;}
static bool file_exists(const char*s){(void)s;return false;}
static bool yesno_pop(const char*s){(void)s;return false;}
void root_menu_agents_shortcut(void){agents_nav_request(AGENTS_NAV_AGENTS,false);}
void root_menu_wps_shortcut(void){agents_nav_request(AGENTS_NAV_WPS,false);}
static int track_result(int ret_val){
TRACK_RESULT
return ret_val;
}
static int make_gesture(int context){
 if(!gesture){struct agents_cover_chord_state s={0};bool fire=false;
 int chord=BUTTON_SELECT|BUTTON_MENU;
 assert(agents_cover_chord_button(&s,chord,chord,true,&fire)==BUTTON_NONE&&fire);
 return ACTION_AGENTS_COVERS;}
 struct agents_remote_state s={0};int found=0;
 for(int i=0;i<3;i++){
  int out=agents_remote_button(&s,context,BUTTON_RC_PLAY,BUTTON_RC_PLAY,100+i*28);
  if(out==REMOTE_COVERS_BUTTON)found++;
  out=agents_remote_button(&s,context,BUTTON_RC_PLAY|BUTTON_REL,0,113+i*28);
  if(out==REMOTE_COVERS_BUTTON)found++;
  while(s.queued){out=agents_remote_button(&s,context,0,0,113+i*28);if(out==REMOTE_COVERS_BUTTON)found++;}
 }
 assert(found==1);return ACTION_AGENTS_REMOTE_COVERS;
}
static int next_action(int context){
 assert(++action_budget<400);
 if(!script && (stage==0 || (stage==1 && cover_nav_active() && agents_nav_has(AGENTS_NAV_TRACKS) && !agents_nav_has(AGENTS_NAV_COVERS)))){
  int remote_context=stage==1?REMOTE_ALBUM_LIST:REMOTE_OTHER;stage++;
  return route_screen_shortcut(make_gesture(remote_context),context);
 }
 if(!script && stage==2){
  int screen=activity==ACTIVITY_AGENTS?GO_TO_AGENTS:activity==ACTIVITY_WPS?GO_TO_WPS:-999;
  assert(screen==expected);assert(agent_view==expected_agent_view&&wps_view==expected_wps_view);returned++;stage=3;
 }
 menus++;return ACTION_STD_MENU;
}
static int quick_screen_current(int a,void(*poll)(void)){
 (void)a;(void)poll;overlay_open=1;
 int out=next_action(CONTEXT_QUICKSCREEN);assert(out==ACTION_STD_CANCEL);overlay_open=0;overlay_closes++;return 0;
}
static void list_do_action(int context,int timeout,struct gui_synclist*l,int*a){
 (void)timeout;(void)l;
 if(stage>=1&&first_browser_depth<0)first_browser_depth=depth;
 if(use_quick&&!script&&(stage==0||stage==1)){quick_screen_current(0,NULL);*a=ACTION_NONE;}else *a=next_action(context);
}
'''.replace('TRACK_RESULT',track_case)
# list_do_action reads the real controller's static depth, declared below.
list_fn=fn(setup,'list_do_action');setup=setup.replace(list_fn,'static void list_do_action(int,int,struct gui_synclist*,int*);')
remote=re.sub(r'^#include .*$', '',(R/'apps-ipod/input/agents_remote.c').read_text(),flags=re.M)
artist_function=fn((R/'apps-ipod/screens/covers/artist_portraits.c').read_text(),'artist_portraits') if not old else ''
body=r'''
static int artist_portraits(const char*);
static int cover_event(int button){
COVER_CASE
}
int cider_covers(void){
 assert(active&&available);assert(!claimed);assert(agents_nav_enter(AGENTS_NAV_COVERS));push_current_activity(ACTIVITY_ALBUMCOVERS);claimed=true;covers_calls++;
 int result;
 if(!script&&stage==1){assert(!overlay_open);if(use_quick){overlay_open=1;stage=2;int out=route_screen_shortcut(make_gesture(REMOTE_QUICKSCREEN),CONTEXT_QUICKSCREEN);assert(out==ACTION_STD_CANCEL);overlay_open=0;overlay_closes++;result=cover_event(ACTION_NONE);}
 else{stage=2;result=cover_event(route_screen_shortcut(make_gesture(REMOTE_COVERS),CONTEXT_PLUGIN));}}
 else if(script==2&&!opened_album){opened_album=1;struct cider_row row={0};strcpy(row.route,"album/picked");strcpy(row.title,"Picked");cider_browse_album(&row);result=GO_TO_PREVIOUS;}
 else{menus++;result=cover_event(ACTION_STD_MENU);}
 claimed=false;pop_current_activity();agents_nav_leave(AGENTS_NAV_COVERS);
 return result==CAROUSEL_REMOTE_RETURN?CIDER_COVERS_REMOTE_RETURN:result;
}
static int album_covers_return(const char*file,bool*toggle){
 (void)file;assert(!active);assert(!claimed);assert(agents_nav_enter(AGENTS_NAV_COVERS));push_current_activity(ACTIVITY_ALBUMCOVERS);claimed=true;covers_calls++;
 int result;
 if(!script&&stage==1){assert(!overlay_open);if(use_quick){overlay_open=1;stage=2;int out=route_screen_shortcut(make_gesture(REMOTE_QUICKSCREEN),CONTEXT_QUICKSCREEN);assert(out==ACTION_STD_CANCEL);overlay_open=0;overlay_closes++;result=cover_event(ACTION_NONE);}
 else{stage=2;result=cover_event(route_screen_shortcut(make_gesture(REMOTE_COVERS),CONTEXT_PLUGIN));}}
 else{menus++;result=cover_event(ACTION_STD_MENU);}
 if(toggle)*toggle=result==CAROUSEL_REMOTE_RETURN;
 claimed=false;pop_current_activity();agents_nav_leave(AGENTS_NAV_COVERS);
 return result==CAROUSEL_REMOTE_RETURN?GO_TO_PREVIOUS:result;
}
static int generic(void*p){
 int entry=(int)(intptr_t)p;bool owner=entry%2==0;assert(!claimed);claimed=owner;
 int out;if(use_quick&&!script&&stage==0){quick_screen_current(0,NULL);out=ACTION_STD_CANCEL;}else out=next_action(CONTEXT_STD);assert(out==ACTION_STD_MENU||out==ACTION_STD_CANCEL);
 claimed=false;return GO_TO_PREVIOUS;
}
static int browser(void*p){
 (void)p;int out;if(use_quick&&!script&&(stage==0||stage==1)){quick_screen_current(0,NULL);out=ACTION_STD_CANCEL;}else out=next_action(CONTEXT_ID3DB);assert(out==ACTION_STD_MENU||out==ACTION_STD_CANCEL);
 return track_result(GO_TO_PREVIOUS);
}
static int agents_screen(void){
 assert(agents_nav_enter(AGENTS_NAV_AGENTS));push_current_activity(ACTIVITY_AGENTS);
 int out,result=0;if(use_quick&&!script&&stage==0){quick_screen_current(0,NULL);out=ACTION_STD_CANCEL;}else out=next_action(CONTEXT_AGENTS);
 if(out==ACTION_AGENTS_COVERS||out==ACTION_AGENTS_REMOTE_COVERS||agents_nav_pending()==AGENTS_NAV_COVERS)result=AGENTS_SWITCH_COVERS;
 pop_current_activity();agents_nav_leave(AGENTS_NAV_AGENTS);return result;
}
static long gui_wps_show(bool return_agents,bool menu_covers){
 (void)return_agents;assert(agents_nav_enter(AGENTS_NAV_WPS));long result=GO_TO_PREVIOUS;
 for(;;){
  int out;if(use_quick&&!script&&stage==0){quick_screen_current(0,NULL);out=ACTION_STD_CANCEL;}else out=next_action(CONTEXT_WPS);
  if(out==ACTION_AGENTS_COVERS||out==ACTION_AGENTS_REMOTE_COVERS){
   cider_remote_source();if(cover_nav_active())cover_nav_finish();cover_nav_begin(GO_TO_WPS);
   long go=GO_TO_WPS;if(show_album_covers(&go,&menu_covers)){result=go;break;}continue;
  }
  if(agents_nav_pending()==AGENTS_NAV_COVERS){agents_nav_take(AGENTS_NAV_COVERS);cider_remote_source();if(cover_nav_active())cover_nav_finish();cover_nav_begin(GO_TO_WPS);long go=GO_TO_WPS;if(show_album_covers(&go,&menu_covers)){result=go;break;}continue;}
  break;
 }
 agents_nav_leave(AGENTS_NAV_WPS);return result;
}
#define CAROUSEL_CONTEXT -1002
static int artist_model,artist_mode;static bool artist_resume_valid;
static int carousel_cover_menu(void){assert(0);return GO_TO_PREVIOUS;}
static int carousel_run(const int*model,const char*file){
 (void)model;(void)file;assert(agents_nav_enter(AGENTS_NAV_COVERS));claimed=true;
 int out=artist_mode?route_screen_shortcut(make_gesture(REMOTE_COVERS),CONTEXT_PLUGIN):ACTION_STD_MENU;
 int result=cover_event(out);claimed=false;agents_nav_leave(AGENTS_NAV_COVERS);return result;
}
struct root_item{int(*function)(void*);void*param;};
static struct root_item items[GO_TO_CIDER+1];
'''.replace('COVER_CASE',cover_case)
root_functions=root_functions.replace('static inline int load_screen','static int load_screen')
model=(R/'apps-ipod/screens/playback/cider_model.c').read_text();model=model[model.index('const char *cider_list_notice('):model.index('/* Compare only song routes;')]
# GUI wrapper calls the actual WPS cover helper, which itself calls our adapter.
prototype='static bool show_album_covers(long*,bool*);\n'
end=r'''
static int next_screen;
static void pending_dispatch(void){
PENDING_DISPATCH
}
static void previous_dispatch(void){
 switch(next_screen){
PREVIOUS_DISPATCH
 default:break;
 }
}
static void run(int screen){
 for(int turns=0;turns<48;turns++){
  if(screen==GO_TO_ROOT){
   if(stage==0&&!script){if(use_quick)quick_screen_current(0,NULL);else next_action(CONTEXT_STD);}
   else return;
  }
  next_screen=screen;pending_dispatch();screen=next_screen;
  int ret=load_screen(screen);next_screen=ret;previous_dispatch();screen=next_screen;
 }
 fprintf(stderr,"MENU cycle: screen=%d last=%d depth=%d\n",screen,last_screen,depth);assert(0);
}
static void reset(int online){
 claimed=false;overlay_closes=0;available=active=online;paused=false;last_screen=GO_TO_ROOT;back_from_wps=false;cover_toggled_back=false;
 nav_live=0;nav_pending=AGENTS_NAV_NONE;cover_journey=cover_music=false;
 stage=0;script=0;menus=returned=covers_calls=source_calls=action_budget=opened_album=0;
 first_browser_depth=-1;depth=0;show_covers=album_pending=false;cover_album_depth=-1;
 RESET_EXTRA
 activity_count=0;activity=ACTIVITY_MAINMENU;overlay_open=0;
 expected_agent_view=agent_view;expected_wps_view=wps_view;
 memset(history,0,sizeof history);for(int i=0;i<12;i++)strcpy(history[i].route,"albums");
}
int main(void){
 for(int i=0;i<=GO_TO_CIDER;i++)items[i]=(struct root_item){generic,(void*)(intptr_t)i};
 items[GO_TO_CIDER]=(struct root_item){cider_scrn,0};items[GO_TO_ALBUM_COVERS]=(struct root_item){album_covers_scrn,0};
 items[GO_TO_ALBUM_COVERS_TRACKS]=(struct root_item){cover_tracks_scrn,(void*)GO_TO_ALBUM_COVERS_TRACKS};
 items[GO_TO_WPS]=(struct root_item){wpsscrn,0};items[GO_TO_AGENTS]=(struct root_item){agents_scrn,0};
 MAIN_CASES
}
'''
start=root.index('void root_menu(void)');a=root.index('#ifdef HAVE_USB_PODLINK',root.index('    while (true)',start));z=root.index('#endif',a)+len('#endif');pending=root[a:z]
a=root.index('            case GO_TO_PREVIOUS:',start);z=root.index('            case GO_TO_PREVIOUS_BROWSER:',a);previous=root[a:z]
end=end.replace('PENDING_DISPATCH',pending).replace('PREVIOUS_DISPATCH',previous)
end=end.replace('RESET_EXTRA','cover_parent_depth=-1;' if not old else 'covers_home=covers_agents=false;')
if old:
 artist_function='static int artist_portraits(const char*p){(void)p;return GO_TO_PREVIOUS;}'
 cases='reset(1);script=1;stage=1;run(GO_TO_ALBUM_COVERS);assert(menus<=16);puts("baseline unexpectedly escaped");'
else:
 cases=r'''
 /* Artist Portraits shares the carousel but owns a different root wrapper.
    Exercise its real unwind and pending dispatch, not just the model adapter. */
 items[GO_TO_ARTIST_PORTRAITS]=(struct root_item){artist_portraits_scrn,0};
 for(gesture=0;gesture<2;gesture++){
  reset(0);artist_mode=1;assert(load_screen(GO_TO_ARTIST_PORTRAITS)==GO_TO_WPS);
  assert(last_screen==GO_TO_ROOT&&!claimed&&!agents_nav_active()&&agents_nav_pending()==AGENTS_NAV_NONE);
 }
 reset(0);artist_mode=0;assert(load_screen(GO_TO_ARTIST_PORTRAITS)==GO_TO_PREVIOUS);assert(last_screen==GO_TO_ROOT);
 items[GO_TO_ARTIST_PORTRAITS]=(struct root_item){generic,(void*)(intptr_t)GO_TO_ARTIST_PORTRAITS};
 int combinations=0;
 for(int online=0;online<2;online++)for(gesture=0;gesture<2;gesture++)for(int quick=0;quick<2;quick++)for(int entry=GO_TO_ROOT;entry<=GO_TO_CIDER;entry++){
  if(entry==GO_TO_ALBUM_COVERS||entry==GO_TO_ALBUM_COVERS_TRACKS)continue;
  reset(online);use_quick=quick;expected=entry==GO_TO_AGENTS?GO_TO_AGENTS:GO_TO_WPS;
  if(entry==GO_TO_CIDER)depth=3;
  run(entry);assert(returned==1&&stage==3&&covers_calls==1);if(use_quick)assert(overlay_closes==2);assert(!claimed&&!agents_nav_active()&&!cover_nav_active()&&activity_count==0);
  combinations++;
 }
 for(int online=0;online<2;online++)for(gesture=0;gesture<2;gesture++)for(int origin=0;origin<3;origin++){
  reset(online);use_quick=false;stage=1;expected=origin==1?GO_TO_AGENTS:GO_TO_WPS;cover_nav_begin(origin==1?GO_TO_AGENTS:origin==2?GO_TO_WPS:GO_TO_ROOT);
  if(online){depth=4;cover_parent_depth=3;cover_album_depth=4;run(GO_TO_CIDER);assert(depth<=3);}
  else run(GO_TO_ALBUM_COVERS_TRACKS);
  assert(returned==1&&stage==3&&covers_calls==0&&!cover_nav_active()&&!claimed&&!agents_nav_active());combinations++;
 }
 for(int parent=0;parent<12;parent++)for(int album=0;album<2;album++){
  if(parent==11&&album){continue;}reset(1);use_quick=false;script=album?2:1;stage=1;depth=parent;
  int saved=depth;cider_browse_covers(false);run(GO_TO_CIDER);
  assert(first_browser_depth==(album?saved+1:saved));assert(depth==0&&cover_album_depth==-1&&cover_parent_depth==-1);
  assert(covers_calls==1+album&&menus<24&&!agents_nav_active()&&!claimed&&!cover_nav_active());
 }
 /* Repeated MENU from every root-dispatchable screen cannot return to an
    automatic transitional Covers node; run() detects any bounded-walk cycle. */
 for(int online=0;online<2;online++)for(int entry=GO_TO_ROOT;entry<=GO_TO_CIDER;entry++){
  reset(online);use_quick=false;script=1;stage=1;run(entry);assert(menus<32&&!claimed&&!agents_nav_active());
 }
 printf("FU43 %d entry/gesture/source and album-toggle combinations; 23 Cider parent/album MENU walks; every root screen bounded MENU PASS\n",combinations);
'''
end=end.replace('MAIN_CASES',cases)
(O/'config.h').write_text('')
source=prefix+pre+setup+nav+chord+remote+route+model+re.sub(r'^#include .*$', '',cider,flags=re.M)+list_fn+prototype+body+artist_function+fn(wps,'show_album_covers')+root_functions+end
(O/'test.c').write_text(source)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-enum-conversion','-Wno-unused-function','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True,timeout=30)
