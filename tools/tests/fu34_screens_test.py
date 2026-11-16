"""FU34 production key lookup, QS loop and screen dispatch with bounded UI adapters.

The expensive carousel renderer and native browser filesystem loop are omitted.
Their production quickscreen dispatch blocks run with the real key tables and
QS loop. Agents' modal/poll/return helpers run for all eight production views.
No hardware, device files or service restart.
"""
from pathlib import Path
import re, subprocess, sys
R=Path(__file__).resolve().parents[2]
O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
S=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else R

def fn(s,n):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]

subprocess.run([sys.executable,str(R/'tools/tests/fu32_quickscreen_test.py'),str(O/'qs'),str(S)],check=True)
base=(O/'qs/test.c').read_text().split('static void add(')[0]
keymap=(S/'apps-ipod/input/keymap_ipod.c').read_text()
keymap=keymap[keymap.index('static const struct button_mapping'):]
action=(S/'apps-ipod/input/action.c').read_text()
carousel=(S/'apps-ipod/screens/covers/carousel.c').read_text()
agents=(S/'apps-ipod/screens/system/agents_screen.c').read_text()
assert 'open_confirm' not in agents and 'VIEW_CONFIRM' not in agents
browser=(S/'apps-ipod/screens/browse/browser.c').read_text()
cbrowser=(S/'apps-ipod/screens/playback/cider_browser.c').read_text()
# Replace the fixture's six tables with all production context cascades.
a=base.index('static const struct button_mapping button_context_standard')
b=base.index('static inline int action_code_worker',a)
base=base[:a]+keymap+'\n'+base[b:]
base=base.replace('#define LAST_ITEM_IN_LIST {0,0,0}', '#define LAST_ITEM_IN_LIST {CONTEXT_STOPSEARCHING,0,0}')
base=base.replace('#define LAST_ITEM_IN_LIST__NEXTLIST(c) {0,0,0}', '#define LAST_ITEM_IN_LIST__NEXTLIST(c) {c,0,0}')
base=base.replace('bool talk_menu;int playlist_shuffle', 'bool talk_menu,hold_lr_for_scroll_in_list,shortcuts_replaces_qs;int playlist_shuffle')
base=base.replace('const struct button_mapping*items;}action_cur_t;', 'const struct button_mapping*items;const struct button_mapping*(*get_context_map)(int);}action_cur_t;')
base=base.replace('static int get_current_activity(void){return ACTIVITY_QUICKSCREEN;}', 'static int activity;static int get_current_activity(void){return activity;}')
base=base.replace('get_action(', 'quickscreen_get_action(')
base=base.replace('static int raw_status,brightness', 'static int last_raw;static int raw_status,brightness')
base=base.replace('static int get_action_statuscode(int*b){(void)b;return raw_status;}', 'static int get_action_statuscode(int*b){if(b)*b=last_raw;return raw_status;}')
base=base.replace('draws++;assert_values();','draws++;assert(cider_quickscreen==source_cider);assert_values();')
# Keep normal enum values distinct; plugin/custom/remote/locked are bit flags.
special={'CONTEXT_PLUGIN':0x10000,'CONTEXT_CUSTOM':0x20000,'CONTEXT_REMOTE':0x40000,'CONTEXT_LOCKED':0x80000,'CONTEXT_STOPSEARCHING':0x100000}
for n,v in special.items():
 base=re.sub(r'^#define '+n+r' .*$', '',base,flags=re.M)
base='\n'.join('#define %s %d'%(n,v) for n,v in special.items())+'\n'+base
existing=set(re.findall(r'\b(?:ACTION_|CONTEXT_|ACTIVITY_)\w+',base[:base.index('struct gui_quickscreen\n')]))
needed=set(re.findall(r'\b(?:ACTION_|CONTEXT_|ACTIVITY_)\w+',keymap+carousel[carousel.index('const struct button_mapping pf_context_album_scroll'):carousel.index('#define LCD_BUF')]+agents[agents.index('enum view'):agents.index('enum row_kind')]))
# Constants in production screen branches.
needed|={'ACTION_WPS_QUICKSCREEN','ACTIVITY_AGENTS','CONTEXT_ID3DB','ACTION_REPEAT'}
missing=sorted(needed-existing-set(special)-{'ACTION_REPEAT'})
pos=base.index('struct gui_quickscreen\n')
base=base[:pos]+'enum {'+','.join(n+'='+str(1000+i) for i,n in enumerate(missing))+'};\n'+base[pos:]
base+='#define DISABLE_ACTION_REMAP\n'+fn(action,'get_next_context')+fn(action,'action_code_lookup')
# Production carousel tables and custom context selector.
start=carousel.index('#define PF_PREV ');end=carousel.index('#define LCD_BUF')
base+='\n#define LAST_ACTION_PLACEHOLDER 5000\n'+carousel[start:end]+fn(carousel,'get_context_map')
pre=r'''
#include "apps-ipod/screens/system/herdremote_model.h"
#define HAVE_USB_PODLINK
#define HAVE_USB_CONFIRM
#include "apps-ipod/screens/system/agents_screen.h"
#define SCREEN_MAIN 0
#define GO_TO_ROOT 2001
#define GO_TO_PREVIOUS 2002
#define GO_TO_WPS 2003
#define GO_TO_SHORTCUTMENU 2004
#define NUM_FILTER_MODES 99
#define Icon_NOICON 0
#define DRMODE_FG 1
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TIME_AFTER(a,b) ((long)((a)-(b))>0)
static int consent_requests,theme_undos;
static void viewportmanager_theme_undo(int screen,bool fullscreen){(void)screen;assert(!fullscreen);theme_undos++;}
static void usb_confirm_reopen(void){assert(theme_undos>0);consent_requests++;}
static int entry_keys[8],entry_count,entry_at,entry_context;
static int entry_action(int context,int raw){
 current_tick+=20;last_raw=raw;raw_status=raw&BUTTON_REPEAT?ACTION_REPEAT:0;
 int key=agents_remote_button(&remote,remote_context(context),raw,raw&BUTTON_REL?0:raw,current_tick);
 if(!key)return ACTION_NONE;
 action_cur_t cur={.button=key,.context=context,.get_context_map=context==CONTEXT_PLUGIN?get_context_map:NULL};
 action_last_t last={prev};
 if(!remote_volume_action(&cur))action_code_lookup(&last,&cur);
 prev=key;return cur.action==ACTION_UNKNOWN?ACTION_NONE:cur.action;
}
static int get_action(int context,int timeout){(void)timeout;assert(context==entry_context&&entry_at<entry_count);return entry_action(context,entry_keys[entry_at++]);}
static int selected=7,top_item=4;
struct gui_synclist {int selected_item,start_item[1];struct viewport*parent[1];};
static int gui_synclist_get_sel_pos(struct gui_synclist*l){return l->selected_item;}
static int gui_synclist_get_nb_lines(struct gui_synclist*l,int s){(void)l;(void)s;return 6;}
static void gui_synclist_select_item(struct gui_synclist*l,int s){l->selected_item=s;}
static void gui_synclist_set_top_item(struct gui_synclist*l,int s){l->start_item[0]=s;}
static void gui_synclist_draw(struct gui_synclist*l){(void)l;}
static bool selectable(int s){return s>=0;}
static int last_view_drawn=-1,page_draws,overlay_active,polls,pumped;
static void show_at(struct gui_synclist*l,int s,int t){l->selected_item=s;l->start_item[0]=t;last_view_drawn=-1;page_draws++;}
static void show(struct gui_synclist*l,int s){show_at(l,s,0);}
static void build_rows(struct gui_synclist*l){(void)l;}
static void draw_usage(struct gui_synclist*l){(void)l;}
static void draw_detail(struct gui_synclist*l){(void)l;}
static void draw_question_overlay(void){}
static void send_key(const char*k){(void)k;assert(0);}
const char*hr_wheel_step(struct hr_wheel*w,long t,int dir,long fast){(void)w;(void)t;(void)dir;(void)fast;assert(0);return NULL;}
#define WHEEL_FAST 1
#define USE_BLOCK 1
#define USE_LINE 1
static struct hr_wheel wheel;
static bool volume_paging,host_keys,volume_enabled,native_play_pending;
static unsigned long native_play_at;
static long pending_until;
static bool pending_answer,question_custom,question_closing,detail_ready=true,talk_held;
static char pending_req[8],focus_req[8],talk_req[8],question_id[12],detail_pane[16],space_id[16],space_tab[16];
static int usage_scroll=3,detail_scroll=5,detail_scroll_max=40,detail_page_lines=8,question_choice;
static struct hr_model model;
static int nrows=12;
struct row {int kind,index;char id[16];};static struct row rows[12];
static void iap_agents_volume_enable(bool b){volume_enabled=b;}
static void iap_agents_volume_host_keys(bool b){host_keys=b;}
static void finish_talk(bool discard){assert(discard);talk_held=false;}
static void pump_keys(void){pumped++;}
static int fixture_result;
static bool poll_link(void){
 assert(!volume_enabled&&!host_keys);polls++;
 if(fixture_result==1){model.result_new=true;model.result_ok=true;strcpy(model.result_req,pending_req);strcpy(model.result_text,"accepted");fixture_result=2;}
 else if(fixture_result==2){model.result_new=true;model.result_ok=false;strcpy(model.result_req,"other");strcpy(model.result_text,"unrelated");fixture_result=3;}
 return false;
}
static void splash(int t,const char*s){(void)t;(void)s;assert(0);}
static void splashf(int t,const char*s,...){(void)t;(void)s;assert(0);}
static void leave_question(struct gui_synclist*l){(void)l;question_closing=false;}
static void enter_answer(struct gui_synclist*l){(void)l;assert(0);}
static void answer_number(int n){(void)n;assert(0);}
static void close_input(bool b){(void)b;assert(0);}
static void start_talk(struct gui_synclist*l,bool r){(void)l;(void)r;assert(0);}
static const struct hr_question*current_question(void){return NULL;}
static bool question_binary_page(void){return false;}
static void want_focus(struct gui_synclist*l){(void)l;assert(0);}
static bool list_do_action(int ctx,int ticks,struct gui_synclist*l,int*a){(void)l;*a=get_action(ctx,ticks);return false;}
#define ANIM_TICKS 5
'''
# The shared fixture uses a tiny viewport; these fields serve the input-only path.
base=base.replace('struct viewport{int dummy;};','struct viewport{int height;};')
pre+=re.search(r'enum view \{[^}]*\};',agents).group()+'\nstatic enum view view;static int input_top;\n'
pre+=(fn(agents,'agents_usb_exit') if 'static void agents_usb_exit(' in agents else 'static void agents_usb_exit(int result){(void)result;}')
pre+=fn(agents,'show_same')+fn(agents,'update_volume_mode')+fn(agents,'take_result')+fn(agents,'question_action')+fn(agents,'agents_menu_exit')
a=agents.index('static struct\n{\n    bool ready, ok;')
b=agents.index('static bool agents_play_down',a)
pre+=agents[a:b]
# Keep production input and early dispatch, including the two input paths.
a=agents.index('        int before = gui_synclist_get_sel_pos(&list);',agents.index('static int agents_run(void)\n{'))
b=agents.index('        switch (action)',a)
cleanup=agents[agents.rindex('    FOR_NB_SCREENS(i)'):agents.rindex('    return ret;')]
pre+='static bool expect_menu_exit;static void run_agents_entry(void){struct gui_synclist list={.selected_item=selected,.start_item={top_item}};int action,ret=0;bool done=false;while(entry_at<entry_count&&!done){\n'+agents[a:b]+'\n}\nassert(done==expect_menu_exit&&!ret);if(done){'+cleanup+'}assert(list.selected_item==selected&&list.start_item[0]==top_item);}\n'
# Native grid browser's actual quickscreen case, with its display/filesystem
# work represented by the caller's restore flag.
case=browser[browser.index('            case ACTION_STD_QUICKSCREEN:'):browser.index('            case ACTION_TREE_HOTKEY:',browser.index('            case ACTION_STD_QUICKSCREEN:'))]
pre+=r'''
static bool reload_dir;static int do_restore_display=7;
static int exit_to_new_screen(int n){return n;}
static int do_shortcut_menu(void*p){(void)p;assert(0);return 0;}
static int run_native_grid(void){int filter=0;struct{int*dirfilter;}tc={&filter};int restore=0;
 while(entry_at<entry_count){int button=get_action(CONTEXT_ID3DB,HZ/5);switch(button){
'''+case+r'''
 default:break;}}
 assert(restore==do_restore_display&&!reload_dir);return 0;
}
'''
# Actual Cider list/grid quickscreen dispatch.
a=cbrowser.index('        if (action == ACTION_STD_QUICKSCREEN)')
b=cbrowser.index('        if (action == ACTION_STD_CANCEL',a)
pre+='static void run_cider_grid(void){int result=0;while(entry_at<entry_count){int action=get_action(CONTEXT_ID3DB,HZ/5);\n'+cbrowser[a:b]+'}\nassert(!result);}\n'
# Carousel production quickscreen case and state retained underneath it.
a=carousel.index('        case ACTION_STD_QUICKSCREEN:',carousel.index('static int album_covers_loop'))
b=carousel.index('        case PF_MENU:',a)
pre+=r'''
static int joins,restarts,title_restores,view_restores;static bool loader_running=true;
static int pf_vp,pf_bg_color,pf_fg_color;
static void end_pf_thread(void){assert(loader_running);loader_running=false;joins++;}
static bool create_pf_thread(void){assert(!loader_running);loader_running=true;restarts++;return true;}
static const char*model_title(void){return "covers";}
static void sb_set_persistent_title(const char*t,int i,int s){(void)t;(void)i;(void)s;title_restores++;}
static void lcd_set_viewport(void*v){(void)v;view_restores++;}
static void lcd_set_background(int c){(void)c;}
static void lcd_set_foreground(int c){(void)c;}
static void lcd_set_drawmode(int m){(void)m;}
static int center_index=11,target=13;static unsigned short surfaces[512];
static int run_covers(void){int ret;while(entry_at<entry_count){int button=get_action(CONTEXT_PLUGIN,HZ/5);switch(button){
'''+carousel[a:b]+r'''
 default:break;}}return 0;}
'''
post=r'''
static void add(int raw){events[limit++]=(struct event){.button=raw};}
static void prepare(int context,int key,bool remote_key){
 step=limit=prev=0;remote=(struct agents_remote_state){0};memset(modes,0,sizeof modes);kind=1;cvol=0;
 brightness=32;saves=tweaks=0;entry_at=0;entry_count=2;entry_context=context;
 entry_keys[0]=key;entry_keys[1]=key|BUTTON_REPEAT;
 add(key|BUTTON_REL); /* original hold release stays in QS */
 add(BUTTON_MENU);add(BUTTON_MENU|BUTTON_REL);
 add(BUTTON_PLAY);add(BUTTON_PLAY|BUTTON_REL);
 if(remote_key){add(BUTTON_RC_PLAY);add(BUTTON_RC_PLAY|BUTTON_REL);}
 else {add(BUTTON_SELECT);add(BUTTON_SELECT|BUTTON_REL);}
}
static void assert_return(void){assert(step==limit&&brightness==32&&applied==32&&!cider_quickscreen);}
int main(void){
 struct int_setting bright_info={1,63,1,backlight_set_brightness};
 struct settings_list bright={.lang_id=4,.setting=&brightness,.flags=F_INT_SETTING,.int_setting=&bright_info};
 struct int_setting left_info={0,1,1,NULL},right_info={0,4,1,NULL};
 struct settings_list left={.lang_id=1,.setting=&global_settings.playlist_shuffle,.flags=F_INT_SETTING,.int_setting=&left_info},right={.lang_id=2,.setting=&global_settings.repeat_mode,.flags=F_INT_SETTING,.int_setting=&right_info};
 for(int i=0;i<12;i++)rows[i].index=i;
 global_settings.qs_items[0]=global_settings.qs_items[3]=&bright;global_settings.qs_items[1]=&left;global_settings.qs_items[2]=&right;
 for(int src=0;src<2;src++)for(int rk=0;rk<2;rk++){
  source_cider=src;int menu=rk?BUTTON_RC_MENU:BUTTON_MENU;
  for(int cover=0;cover<3;cover++){
   activity=ACTIVITY_ALBUMCOVERS;prepare(CONTEXT_PLUGIN,menu,rk);
   for(int i=0;i<512;i++)surfaces[i]=i+1;
   int joined=joins;assert(!run_covers());assert_return();
   assert(joins==joined+1&&restarts==joins&&loader_running&&center_index==11&&target==13);
   for(int i=0;i<512;i++)assert(surfaces[i]==i+1);
   printf("covers provider=%d playback=%s key=%s: QS -> identical center/target/cache, loader restart PASS\n",cover,src?"Cider":"local",rk?"remote":"body");
  }
  for(int grid=0;grid<2;grid++){
   activity=ACTIVITY_DATABASEBROWSER;prepare(CONTEXT_ID3DB,menu,rk);
   if(grid)run_cider_grid();else run_native_grid();assert_return();
   printf("%s album grid playback=%s key=%s: QS -> same caller PASS\n",grid?"Cider":"local",src?"Cider":"local",rk?"remote":"body");
  }
  for(int v=VIEW_SPACES;v<=VIEW_INPUT;v++){
   view=v;strcpy(space_id,"w1");strcpy(space_tab,"t1");strcpy(detail_pane,"p1");strcpy(question_id,"q1");question_choice=2;
   pending_req[0]=0;model.hello=true;model.nws=2;volume_paging=false;update_volume_mode();
   activity=ACTIVITY_AGENTS;prepare(CONTEXT_AGENTS,menu,rk);int before=polls;
   run_agents_entry();assert_return();
   assert(view==v&&!strcmp(space_id,"w1")&&!strcmp(space_tab,"t1")&&!strcmp(detail_pane,"p1")&&!strcmp(question_id,"q1")&&question_choice==2);
   assert(model.hello&&model.nws==2&&usage_scroll==3&&detail_scroll==5&&polls>before&&pumped==polls);
   assert(volume_enabled==(v!=VIEW_USAGE)&&host_keys==(v==VIEW_DETAIL||v==VIEW_USAGE));
   printf("Agents view=%d playback=%s key=%s: QS -> same selection/top/scroll/session, foreground polling PASS\n",v,src?"Cider":"local",rk?"remote":"body");
  }
 }
 view=VIEW_AGENTS;source_cider=true;activity=ACTIVITY_AGENTS;prepare(CONTEXT_AGENTS,BUTTON_MENU,false);
 strcpy(pending_req,"req1");pending_until=current_tick+100;fixture_result=1;
 run_agents_entry();assert(fixture_result==3&&!pending_req[0]&&!model.result_new);
 puts("Agents pending result retained across later unrelated result; consumed after QS PASS");

 for(int v=VIEW_SPACES;v<=VIEW_INPUT;v++)for(int rk=0;rk<2;rk++){
  view=v;source_cider=true;activity=ACTIVITY_AGENTS;
  prepare(CONTEXT_AGENTS,rk?BUTTON_RC_MENU:BUTTON_MENU,rk);
  entry_keys[1]=entry_keys[0]|BUTTON_REL;expect_menu_exit=true;
  int before=cleans,consent_before=consent_requests;model.hello=true;model.nws=2;
  run_agents_entry();assert(consent_requests==consent_before+1);assert(!step&&cleans==before&&model.hello&&model.nws==2&&view==v);
  /* A pending answer/focus/voice request does not intercept MENU. */
  pending_answer=true;strcpy(pending_req,"pending");strcpy(focus_req,"focus");strcpy(talk_req,"voice");
  prepare(CONTEXT_AGENTS,rk?BUTTON_RC_MENU:BUTTON_MENU,rk);entry_keys[1]=entry_keys[0]|BUTTON_REL;
  run_agents_entry();assert(consent_requests==consent_before+2);assert(!step&&!strcmp(pending_req,"pending")&&view==v);
  pending_answer=false;pending_req[0]=focus_req[0]=talk_req[0]=0;
  printf("Agents view=%d key=%s: short MENU -> direct previous-screen exit, no Leave confirm, one USB consent, no QS/session reset PASS\n",v,rk?"remote":"body");
 }
 last_raw=BUTTON_MENU|BUTTON_REL;assert(agents_menu_exit(ACTION_STD_CANCEL));
 last_raw=BUTTON_LEFT;assert(!agents_menu_exit(ACTION_STD_CANCEL));
 expect_menu_exit=false;
 assert(title_restores==joins&&view_restores==joins);
 puts("FU34 production dispatch/key/QS matrix: 12 covers + 8 grids + 28 Agents cases PASS");
}
'''
# Test fixture adaptations deliberately do not alter production dispatch code.
pre=pre.replace('static struct hr_model model;', 'static struct hr_model model;')
# Native browser accesses a normal global_status field.
base=base.replace('static struct {int volume;}global_status;', 'static struct {int volume,last_screen;}global_status;')
(O/'screen.c').write_text(base+pre+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-sign-compare','-Wno-enum-compare','-Wno-pointer-sign','-Wno-implicit-fallthrough','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O/'qs'),'-I'+str(R),str(O/'screen.c'),'-o',str(O/'screen')],check=True)
subprocess.run([str(O/'screen')],check=True)
