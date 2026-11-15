"""FU32 real keymap worker, remote decoder, quickscreen loop and skin QS tokens."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True);S=Path(sys.argv[2]) if len(sys.argv)>2 else R
def fn(s,n):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
q=(S/'apps-ipod/screens/playback/quick_screen.c').read_text();a=(S/'apps-ipod/input/action.c').read_text();k=(S/'apps-ipod/input/keymap_ipod.c').read_text();skin=(S/'apps-ipod/skin/skin_tokens.c').read_text()
names=['button_context_standard','button_context_wps','button_context_quickscreen','remote_button_context_standard','remote_button_context_wps','remote_button_context_quickscreen']
tables='\n'.join(re.search(r'static const struct button_mapping '+n+r'\[\].*?\n};',k,re.S).group() for n in names)
actions=sorted(set(re.findall(r'\bACTION_\w+',tables+fn(q,'gui_syncquickscreen_run')+fn(q,'gui_quickscreen_do_button')))|{'ACTION_UNKNOWN'})
tokens=sorted(set(re.findall(r'\bSKIN_TOKEN_\w+',fn(skin,'get_qs_token_value'))))
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IPOD_ACCESSORY_PROTOCOL
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#include "firmware/target/arm/s5l8702/ipod6g/button-target.h"
#include "apps-ipod/input/agents_remote.h"
#define HZ 100
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for(int i=0;i<NB_SCREENS;i++)
#define LAST_ITEM_IN_LIST {0,0,0}
#define LAST_ITEM_IN_LIST__NEXTLIST(c) {0,0,0}
#define has_flag(a,b) (((a)&(b))==(b))
#define P2STR(p) ((const char*)(p))
#define ID2P(n) ((const unsigned char*)((n)==1?"Shuffle":"Repeat"))
#define SYS_EVENT 0x80000000u
#define BUTTON_MULTIMEDIA 0x10000000
#define CONTEXT_STD 1
#define CONTEXT_WPS 2
#define CONTEXT_QUICKSCREEN 3
#define CONTEXT_AGENTS 4
#define CONTEXT_PLUGIN 5
#define ACTIVITY_ALBUMCOVERS 8
#define ACTIVITY_QUICKSCREEN 9
#define ACTIVITY_DATABASEBROWSER 10
#define AGENTS_NAV_TRACKS 1
#define CIDER_MODE_SHUFFLE 0
#define CIDER_MODE_REPEAT 1
#define SOUND_VOLUME 0
#define UNIT_DB 0
#define VOICE_QUICKSCREEN 0
#define VOICE_OK 0
#define SYS_USB_CONNECTED 77
#define GUI_EVENT_NEED_UI_UPDATE 1
#define QUICKSCREEN_OK 0
#define QUICKSCREEN_IN_USB 1
#define QUICKSCREEN_CHANGED 4
#define QUICKSCREEN_GOTO_SHORTCUTS_MENU 2
#define QUICKSCREEN_TOP 0
#define QUICKSCREEN_LEFT 1
#define QUICKSCREEN_RIGHT 2
#define QUICKSCREEN_BOTTOM 3
#define QUICKSCREEN_ITEM_COUNT 4
#define LANG_VOLUME 3
struct viewport{int dummy;};
enum screen_type{SCREEN_MAIN};enum quickscreen_item{dummy_item};
struct settings_list {int lang_id;int*setting;};
static struct {bool talk_menu;int playlist_shuffle,repeat_mode;const struct settings_list*qs_items[4];}global_settings;
static struct {int volume;}global_status;
static int modes[2],cvol,localsteps;
static bool source_cider;
static bool cider_active(void){return source_cider;}
static int get_current_activity(void){return ACTIVITY_QUICKSCREEN;}
static bool agents_nav_has(int n){(void)n;return false;}
static int cider_mode_get(int m){return modes[m];}
static void cider_mode_set(int m,int v){modes[m]=v;}
static int cider_mode_step(int m,int v,bool prev){int n=m?5:2;return(v+(prev?n-1:1))%n;}
static const char*cider_mode_title(int m){return m?"Repeat":"Shuffle";}
static const char*cider_mode_label(int m,int v){static const char*repeat[]={"Off","All","One","Shuffle","A-B"};return m?repeat[v]:v?"On":"Off";}
static void cider_control(const char*c,int v){assert(!strcmp(c,"volume-step"));cvol+=v;}
static void adjust_volume(int n){localsteps+=n;}
static void option_select_next_val(const struct settings_list*s,bool prev,bool apply){(void)apply;int n=s->lang_id==1?2:5;*s->setting=(*s->setting+(prev?n-1:1))%n;}
static int option_value_as_int(const struct settings_list*s){return *s->setting;}
static const char*option_get_valuestring(const struct settings_list*s,char*b,int n,int v){(void)b;(void)n;return cider_mode_label(s->lang_id==2,v);}
static const struct settings_list *find_setting(const void*p){for(int i=0;i<4;i++)if(global_settings.qs_items[i]&&global_settings.qs_items[i]->setting==p)return global_settings.qs_items[i];return NULL;}
struct button_mapping{int action_code,button_code,pre_button_code;};
typedef struct{int button;}action_last_t;
typedef struct{int button,context,action;bool is_prebutton;const struct button_mapping*items;}action_cur_t;
struct wps_token{int type;};
static void *set_viewport(void*p){(void)p;return NULL;}static void noop(void){}
static struct {void*(*set_viewport)(void*);void(*scroll_stop)(void);}screens[1]={{set_viewport,noop}};
static int step,limit,prev,draws,cleans,kind;
struct event{int button,left,right,volume;};static struct event events[40];
static struct agents_remote_state remote;
static unsigned long current_tick;
static void remote_log_event(int a,unsigned long b,int c,int d,const void*e,unsigned f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
#define REMOTE_LOG_TAP 1
#define REMOTE_LOG_DOUBLE 2
#define REMOTE_LOG_HOLD 3
#define REMOTE_LOG_TRIPLE 4
'''
pre+='enum {'+','.join(actions)+'};\nenum {'+','.join(tokens)+'};\n'
start=q.index('struct gui_quickscreen\n');end=q.index('\n};',start)+3
pre+=q[start:end]+'\nstatic bool cider_quickscreen;\n'
pre+=fn(q,'quick_screen_cider_active') if 'bool quick_screen_cider_active(' in q else 'static bool quick_screen_cider_active(void){return source_cider;}\n'
pre+=tables+fn(a,'action_code_worker')+fn(a,'remote_context')+fn(a,'remote_volume_action')
pre+=re.sub(r'^#include .*\n','',(S/'apps-ipod/input/agents_remote.c').read_text(),flags=re.M)
pre+=fn(skin,'get_qs_token_value')
pre+=r'''
static void assert_values(void){char b[80];struct wps_token t={SKIN_TOKEN_LEFT_QUICKSETTING_VALUE};
 if(kind==0){const char*v=get_qs_token_value(&t,b,sizeof b);assert(!strcmp(v,cider_mode_label(0,modes[0])));t.type=SKIN_TOKEN_RIGHT_QUICKSETTING_VALUE;assert(!strcmp(get_qs_token_value(&t,b,sizeof b),cider_mode_label(1,modes[1])));}}
static void quickscreen_setup_viewports(struct gui_quickscreen*q,int i){(void)q;(void)i;}
static void gui_quickscreen_draw(struct gui_quickscreen*q,int i){(void)q;(void)i;draws++;assert_values();}
static void quickscreen_update(struct gui_quickscreen*q,int i){(void)q;(void)i;draws++;assert_values();}
static void quickscreen_cleanup(void*p){(void)p;cleans++;cider_quickscreen=false;}
static void quickscreen_draw_cb(unsigned short id,void*d,void*u){(void)id;(void)d;(void)u;}
static void push_current_activity(int a){(void)a;}
static void viewportmanager_theme_enable(int i,bool b,struct viewport*p){(void)i;(void)b;(void)p;}
static void add_event_ex(int i,bool b,void(*cb)(unsigned short,void*,void*),void*p){(void)i;(void)b;(void)cb;(void)p;}
static int default_event_handler_ex(int b,void(*cleanup)(void*),void*p){(void)b;(void)cleanup;(void)p;return 0;}
static int button_status(void){return step?events[step-1].button&~(BUTTON_REL|BUTTON_REPEAT):0;}
static void cond_talk_ids(int a){(void)a;}
static void cond_talk_ids_fq(int a){(void)a;}
static void talk_qs_option(struct gui_quickscreen*q,int i,bool b){(void)q;(void)i;(void)b;}
static void talk_settle(void){}static void talk_value_decimal(int a,int b,int c,bool d){(void)a;(void)b;(void)c;(void)d;}
static int sound_val2phys(int a,int b){(void)a;return b;}static int sound_numdecimals(int a){(void)a;return 0;}
static void sb_skin_force_next_update(void){}
static void check_previous(void){if(!step)return;struct event*e=&events[step-1];assert(modes[0]==e->left&&modes[1]==e->right&&cvol==e->volume);}
static int get_action(int context,int timeout){(void)timeout;assert(context==CONTEXT_QUICKSCREEN);check_previous();assert(step<limit);
 int raw=events[step++].button;current_tick+=20;
 int key=agents_remote_button(&remote,remote_context(context),raw,raw&BUTTON_REL?0:raw,current_tick);
 action_cur_t c={.button=key,.context=context};int result=ACTION_NONE;
 if(remote_volume_action(&c))result=c.action;
 else if(key){c.items=key&BUTTON_REMOTE?remote_button_context_quickscreen:button_context_quickscreen;action_last_t l={prev};int i=0;result=action_code_worker(&l,&c,&i);if(result==ACTION_UNKNOWN)result=ACTION_NONE;}
 prev=key;return result;}
'''
pre+=fn(q,'cider_slot_mode')+fn(q,'gui_quickscreen_do_button')+fn(q,'gui_syncquickscreen_run')+fn(q,'quick_screen_cider')
post=r'''
static void add(int key,int l,int r,int v){events[limit++]=(struct event){key,l,r,v};}
static void reset(void){step=limit=prev=0;memset(modes,0,sizeof modes);remote=(struct agents_remote_state){0};cvol=0;}
int main(void){
 struct settings_list left={1,&global_settings.playlist_shuffle},right={2,&global_settings.repeat_mode};global_settings.qs_items[1]=&left;global_settings.qs_items[2]=&right;
 source_cider=true;kind=0;reset();
 add(BUTTON_LEFT,1,0,0);add(BUTTON_LEFT|BUTTON_REL,1,0,0);
 add(BUTTON_RIGHT,1,1,0);add(BUTTON_RIGHT|BUTTON_REL,1,1,0);
 add(BUTTON_SCROLL_FWD,1,1,1);add(BUTTON_MENU,1,1,1);add(BUTTON_MENU|BUTTON_REL,1,1,1);
 add(BUTTON_RC_LEFT,0,1,1);add(BUTTON_RC_LEFT|BUTTON_REPEAT,0,1,1);add(BUTTON_RC_LEFT|BUTTON_REL,0,1,1);
 add(BUTTON_RC_RIGHT,0,2,1);add(BUTTON_RC_RIGHT|BUTTON_REL,0,2,1);
 add(BUTTON_RC_VOL_UP,0,2,1);add(BUTTON_RC_VOL_DOWN,0,2,1);
 add(BUTTON_SELECT,0,2,1);add(BUTTON_SELECT|BUTTON_REL,0,2,1);
 quick_screen_cider(ACTION_WPS_QUICKSCREEN);check_previous();assert(step==limit&&cleans==1);assert(!global_settings.playlist_shuffle&&!global_settings.repeat_mode);
 for(int remote_exit=0;remote_exit<2;remote_exit++){
  reset();kind=1;struct gui_quickscreen qs={0};bool usb;qs.volume_item=4;
  add(BUTTON_MENU,0,0,0);add(BUTTON_MENU|BUTTON_REL,0,0,0); /* empty TOP must not exit */
  int key=remote_exit?BUTTON_RC_PLAY:BUTTON_RC_MENU;add(key,0,0,0);add(key|BUTTON_REL,0,0,0);
  gui_syncquickscreen_run(&qs,ACTION_WPS_QUICKSCREEN,&usb);assert(step==limit&&!usb);
 }
 for(int remote_exit=0;remote_exit<2;remote_exit++){
  reset();kind=0;int key=remote_exit?BUTTON_RC_PLAY:BUTTON_RC_MENU;
  add(BUTTON_MENU,0,0,0);add(BUTTON_MENU|BUTTON_REL,0,0,0);
  add(key,0,0,0);add(key|BUTTON_REL,0,0,0);
  quick_screen_cider(ACTION_WPS_QUICKSCREEN);assert(step==limit);
 }
 reset();kind=1;global_settings.playlist_shuffle=0;struct gui_quickscreen local={0};bool usb;
 local.items[QUICKSCREEN_TOP]=&left;local.items[QUICKSCREEN_BOTTOM]=&right;
 add(BUTTON_MENU,0,0,0);add(BUTTON_MENU|BUTTON_REL,0,0,0);
 add(BUTTON_PLAY,0,0,0);add(BUTTON_PLAY|BUTTON_REL,0,0,0);
 add(BUTTON_RC_PLAY,0,0,0);add(BUTTON_RC_PLAY|BUTTON_REL,0,0,0);
 gui_syncquickscreen_run(&local,ACTION_WPS_QUICKSCREEN,&usb);
 assert(step==limit&&global_settings.playlist_shuffle==1&&global_settings.repeat_mode==4);
 for(int remote_key=0;remote_key<2;remote_key++){
  const struct button_mapping*map=remote_key?remote_button_context_wps:button_context_wps;
  int play=remote_key?BUTTON_RC_PLAY:BUTTON_PLAY;
  action_last_t l={play};action_cur_t c={.button=play|BUTTON_REPEAT,.items=map};int i=0;
  assert(action_code_worker(&l,&c,&i)==ACTION_WPS_CONTEXT);
  int keys[]={remote_key?BUTTON_RC_LEFT:BUTTON_LEFT,remote_key?BUTTON_RC_RIGHT:BUTTON_RIGHT};
  for(int side=0;side<2;side++){l.button=keys[side];c=(action_cur_t){.button=keys[side]|BUTTON_REL,.items=map};i=0;
   assert(action_code_worker(&l,&c,&i)==(side?ACTION_WPS_SKIPNEXT:ACTION_WPS_SKIPPREV));}
 }

 puts("FU32 production key worker + quickscreen loop + themed Cider values: device/remote arrows, wheel, physical MENU TOP, remote MENU/centre exit, Play-hold context PASS");
}
'''
(O/'test.c').write_text(pre+post);(O/'config.h').write_text('')
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
