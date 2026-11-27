"""FU32/FU34 production key worker, quickscreen, native steps and displayed values."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True);S=Path(sys.argv[2]) if len(sys.argv)>2 else R
def fn(s,n):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
q=(S/'apps-ipod/screens/playback/quick_screen.c').read_text();a=(S/'apps-ipod/input/action.c').read_text();k=(S/'apps-ipod/input/keymap_ipod.c').read_text()
names=['button_context_standard','button_context_wps','button_context_quickscreen','remote_button_context_standard','remote_button_context_wps','remote_button_context_quickscreen']
tables='\n'.join(re.search(r'static const struct button_mapping '+n+r'\[\].*?\n};',k,re.S).group() for n in names)
actions=sorted(set(re.findall(r'\bACTION_\w+',tables+fn(q,'gui_syncquickscreen_run')+fn(q,'gui_quickscreen_do_button')))|{'ACTION_UNKNOWN'})
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HAVE_USB_PODLINK
#define IPOD_ACCESSORY_PROTOCOL
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#include "firmware/target/arm/s5l8702/ipod6g/button-target.h"
#include "apps-ipod/input/agents_remote.h"
#define HZ 100
#define MAX_PATH 260
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for(int i=0;i<NB_SCREENS;i++)
#define LAST_ITEM_IN_LIST {0,0,0}
#define LAST_ITEM_IN_LIST__NEXTLIST(c) {0,0,0}
#define has_flag(a,b) (((a)&(b))==(b))
#define P2STR(p) ((const char*)(p))
#define ID2P(n) ((const unsigned char*)((n)==1?"Shuffle":(n)==2?"Repeat":"Brightness"))
#define SYS_EVENT 0x80000000u
#define BUTTON_MULTIMEDIA 0x10000000
#define CONTEXT_STD 1
#define CONTEXT_WPS 2
#define CONTEXT_QUICKSCREEN 3
#define CONTEXT_AGENTS 4
#define CONTEXT_PLUGIN 5
#define ACTIVITY_LYRICS 99
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
#define QUICKSCREEN_RESTORED 8
#define QUICKSCREEN_GOTO_SHORTCUTS_MENU 2
#define QUICKSCREEN_TOP 0
#define QUICKSCREEN_LEFT 1
#define QUICKSCREEN_RIGHT 2
#define QUICKSCREEN_BOTTOM 3
#define QUICKSCREEN_ITEM_COUNT 4
#define LANG_VOLUME 3
struct viewport{int dummy;};
enum screen_type{SCREEN_MAIN};enum quickscreen_item{dummy_item};
#define F_BOOL_SETTING 1
#define F_INT_SETTING 2
#define F_T_SOUND 4
#define F_CHOICE_SETTING 8
#define F_TABLE_SETTING 16
#define F_ALLOW_ARBITRARY_VALS 32
#define HASFLAG(s,f) (((s)->flags&(f))!=0)
#define ACTION_REPEAT 128
struct int_setting {int min,max,step;void(*option_callback)(int);};
struct bool_setting {void(*option_callback)(bool);};
struct choice_setting {int count;void(*option_callback)(int);};
struct table_setting {int count;int*values;void(*option_callback)(int);};
struct sound_setting {int setting;};
struct settings_list {int lang_id;int*setting;int flags;const struct int_setting*int_setting;const struct bool_setting*bool_setting;const struct choice_setting*choice_setting;const struct table_setting*table_setting;const struct sound_setting*sound_setting;};
static const struct settings_list*settings;
static int raw_status,brightness,applied=-1,saves,tweaks;
static int get_action_statuscode(int*b){(void)b;return raw_status;}
static int sound_steps(int i){(void)i;assert(0);return 0;}
static int sound_min(int i){(void)i;assert(0);return 0;}
static int sound_max(int i){(void)i;assert(0);return 0;}
static void sound_set(int i,int v){(void)i;(void)v;assert(0);}
static void backlight_set_brightness(int v){applied=v;}
static bool is_setting_quickscreenable(const struct settings_list*s){(void)s;return true;}
static void settings_mark_user_tweak(const struct settings_list*s){assert(s);tweaks++;}
static void settings_save(void){saves++;}
static struct {bool talk_menu;int shuffle_mode,repeat_mode;const struct settings_list*qs_items[4];}global_settings;
#define playlist_shuffle shuffle_mode
static struct {int volume;long last_volume_change;}global_status;
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
PRODUCTION_OPTION_STEP
static int option_value_as_int(const struct settings_list*s){return *s->setting;}
static const char*option_get_valuestring(const struct settings_list*s,char*b,int n,int v){if(s->lang_id==4){snprintf(b,n,"%d",v);return b;}return cider_mode_label(s->lang_id==2,v);}
static const struct settings_list *find_setting(const void*p){for(int i=0;i<4;i++)if(global_settings.qs_items[i]&&global_settings.qs_items[i]->setting==p)return global_settings.qs_items[i];return NULL;}
struct button_mapping{int action_code,button_code,pre_button_code;};
typedef struct{int button;}action_last_t;
typedef struct{int button,context,action;bool is_prebutton;const struct button_mapping*items;}action_cur_t;
struct wps_token{int type;};
static void *set_viewport(void*p){(void)p;return NULL;}static void noop(void){}
static char fallback_lines[2][260];
static void putline(int x,int y,const char*s){assert(x==0&&y>=0&&y<2);snprintf(fallback_lines[y],sizeof fallback_lines[y],"%s",s);}
struct screen {void*(*set_viewport)(void*);void(*scroll_stop)(void);void(*puts_scroll)(int,int,const char*);};
static struct screen screens[1]={{set_viewport,noop,putline}};
static int step,limit,prev,draws,cleans,kind;
struct event{int button,left,right,volume,expected_brightness;};static struct event events[40];
static struct agents_remote_state remote;
static unsigned long current_tick;
static void remote_log_event(int a,unsigned long b,int c,int d,const void*e,unsigned f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
#define REMOTE_LOG_TAP 1
#define REMOTE_LOG_DOUBLE 2
#define REMOTE_LOG_HOLD 3
#define REMOTE_LOG_TRIPLE 4
'''
pre+='enum {'+','.join(actions)+'};\n'
start=q.index('struct gui_quickscreen\n');end=q.index('\n};',start)+3
pre+=q[start:end]+'\nstatic bool cider_quickscreen;\n'
pre+=fn(q,'quick_screen_cider_active') if 'bool quick_screen_cider_active(' in q else 'static bool quick_screen_cider_active(void){return source_cider;}\n'
pre+=tables+fn(a,'action_code_worker')+fn(a,'remote_context')+fn(a,'remote_volume_action')
pre+=re.sub(r'^#include .*\n','',(S/'apps-ipod/input/agents_remote.c').read_text(),flags=re.M)
pre+=fn(q,'cider_slot_mode')+fn(q,'cider_slot')+fn(q,'quickscreen_title')+fn(q,'quickscreen_value')
pre+=r'''
struct quickscreen_page { struct { bool active; } sheet; int sheet_handle; bool wheel, full_page; };
static void quickscreen_update_row(struct gui_quickscreen*q,int row){(void)q;(void)row;draws++;}
static void quickscreen_wheel_step(struct gui_quickscreen*q,int b){(void)q;(void)b;}
static void quickscreen_wheel_idle(struct gui_quickscreen*q){(void)q;}
static bool quickscreen_wheel_transport(const struct gui_quickscreen*q,int b){(void)q;(void)b;return false;}
static bool ui_flush_inhibited(void){return false;}
static void ui_inhibit_flush(bool on){assert(!on);}
static int ui_sheet_timeout(void *sheet,int timeout){(void)sheet;return timeout;}
static void default_event_handler(int action){(void)action;}
static bool album_palette_needs_repaint(void){return false;}
static bool ui_take_dirty(int screen){(void)screen;return false;}
static void quickscreen_chrome(struct quickscreen_page*v){(void)v;}
static void quickscreen_present(struct quickscreen_page*v,bool capture){(void)v;(void)capture;assert(0);}
static void quickscreen_update(struct gui_quickscreen*q,enum quickscreen_item item)
{
 (void)item;draws++;
 char value[80],expected[40];
 if(q->items[QUICKSCREEN_TOP]&&q->items[QUICKSCREEN_TOP]->lang_id==4){
  snprintf(expected,sizeof expected,"%d",brightness);
  assert(!strcmp(quickscreen_title(q,QUICKSCREEN_TOP),"Brightness"));
  assert(!strcmp(quickscreen_value(q,QUICKSCREEN_TOP,value,sizeof value),expected));
 }
 if(q->cider&&kind==0){
  assert(!strcmp(quickscreen_value(q,QUICKSCREEN_LEFT,value,sizeof value),cider_mode_label(0,modes[0])));
  assert(!strcmp(quickscreen_value(q,QUICKSCREEN_RIGHT,value,sizeof value),cider_mode_label(1,modes[1])));
 }
}
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
static void check_previous(void){if(!step)return;struct event*e=&events[step-1];assert(modes[0]==e->left&&modes[1]==e->right&&cvol==e->volume);if(e->expected_brightness)assert(brightness==e->expected_brightness&&applied==brightness);}
static int get_action(int context,int timeout){(void)timeout;assert(context==CONTEXT_QUICKSCREEN);check_previous();assert(step<limit);
 int raw=events[step++].button;current_tick+=20;
 raw_status=raw&BUTTON_REPEAT?ACTION_REPEAT:0;int key=agents_remote_button(&remote,remote_context(context),raw,raw&BUTTON_REL?0:raw,current_tick);
 action_cur_t c={.button=key,.context=context};int result=ACTION_NONE;
 if(remote_volume_action(&c))result=c.action;
 else if(key){c.items=key&BUTTON_REMOTE?remote_button_context_quickscreen:button_context_quickscreen;action_last_t l={prev};int i=0;result=action_code_worker(&l,&c,&i);if(result==ACTION_UNKNOWN)result=ACTION_NONE;}
 prev=key;return result;}
'''
pre=pre.replace('PRODUCTION_OPTION_STEP',fn((S/'apps-ipod/widgets/option_select.c').read_text() if (S/'apps-ipod/widgets/option_select.c').exists() else (R/'apps-ipod/widgets/option_select.c').read_text(),'option_select_next_val'))
loop=fn(q,'gui_syncquickscreen_run')
start=loop.index('    qs->view_handle =')
end=loop.index('    while (true)',start)
loop=loop[:start]+'''    struct quickscreen_page view_storage={0}, *view=&view_storage;
    qs->view=view;
    quickscreen_update(qs, QUICKSCREEN_ITEM_COUNT);
'''+loop[end:]
start=loop.index('    if (view->sheet_handle > 0)')
end=loop.index('    /* Notify that',start)
loop=loop[:start]+loop[end:]
pre+=fn(q,'gui_quickscreen_do_button')+loop
if 'static int quick_screen_run(' in q:
 pre+=fn(q,'quick_screen_run')+fn(q,'quick_screen_quick')+fn(q,'quick_screen_cider')+fn(q,'quick_screen_current')
else:
 pre+=fn(q,'quick_screen_quick')+fn(q,'quick_screen_cider')
 pre+='static int quick_screen_current(int b,void(*p)(void)){(void)p;return cider_active()?quick_screen_cider(b):quick_screen_quick(b);}'
post=r'''
static void add(int key,int l,int r,int v){events[limit++]=(struct event){key,l,r,v};}
static void addb(int key,int value){add(key,0,0,0);events[limit-1].expected_brightness=value;}
static void reset(void){step=limit=prev=0;memset(modes,0,sizeof modes);remote=(struct agents_remote_state){0};cvol=0;}
int main(void){
 struct int_setting left_info={0,1,1,NULL},right_info={0,4,1,NULL};struct settings_list left={.lang_id=1,.setting=&global_settings.playlist_shuffle,.flags=F_INT_SETTING,.int_setting=&left_info},right={.lang_id=2,.setting=&global_settings.repeat_mode,.flags=F_INT_SETTING,.int_setting=&right_info};global_settings.qs_items[1]=&left;global_settings.qs_items[2]=&right;
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
  assert(action_code_worker(&l,&c,&i)==(remote_key?ACTION_WPS_LYRICS:ACTION_WPS_CONTEXT));
  int keys[]={remote_key?BUTTON_RC_LEFT:BUTTON_LEFT,remote_key?BUTTON_RC_RIGHT:BUTTON_RIGHT};
  for(int side=0;side<2;side++){l.button=keys[side];c=(action_cur_t){.button=keys[side]|BUTTON_REL,.items=map};i=0;
   assert(action_code_worker(&l,&c,&i)==(side?ACTION_WPS_SKIPNEXT:ACTION_WPS_SKIPPREV));}
 }


 /* FU34: the same native brightness setting and option implementation serve
  * TOP/BOTTOM in both sources. Each physical step and display is checked. */
 struct int_setting bright_info={1,63,1,backlight_set_brightness};
 struct settings_list bright={.lang_id=4,.setting=&brightness,.flags=F_INT_SETTING,.int_setting=&bright_info};
 global_settings.qs_items[QUICKSCREEN_TOP]=global_settings.qs_items[QUICKSCREEN_BOTTOM]=&bright;
 for(int src=0;src<2;src++)for(int keysource=0;keysource<2;keysource++)for(int exitkey=0;exitkey<3;exitkey++){
  source_cider=src;kind=1;reset();brightness=32;applied=-1;saves=tweaks=0;
  int up=keysource?BUTTON_RC_VOL_UP:BUTTON_MENU,down=keysource?BUTTON_RC_VOL_DOWN:BUTTON_PLAY;
  addb(up,33);addb(up|BUTTON_REPEAT,34);addb(up|BUTTON_REL,34);
  addb(down,33);addb(down|BUTTON_REL,33);
  int done=exitkey==0?BUTTON_SELECT:exitkey==1?BUTTON_RC_MENU:BUTTON_RC_PLAY;
  add(done,0,0,0);add(done|BUTTON_REL,0,0,0);
  quick_screen_current(ACTION_STD_QUICKSCREEN,NULL);check_previous();
  assert(step==limit&&brightness==33&&applied==33&&saves==1&&tweaks==0);
  struct gui_quickscreen values={.cider=src};
  values.items[QUICKSCREEN_TOP]=values.items[QUICKSCREEN_BOTTOM]=&bright;
  char value[80];
  assert(!strcmp(quickscreen_title(&values,QUICKSCREEN_TOP),"Brightness"));
  assert(!strcmp(quickscreen_value(&values,QUICKSCREEN_BOTTOM,value,sizeof value),"33"));
 }
 for(int src=0;src<2;src++){
  /* Limits match native repeated and non-repeated presses, step=1. */
  source_cider=src;kind=1;reset();brightness=63;saves=tweaks=0;
  add(BUTTON_MENU|BUTTON_REPEAT,0,0,0);add(BUTTON_MENU,0,0,0);
  add(BUTTON_SELECT,0,0,0);add(BUTTON_SELECT|BUTTON_REL,0,0,0);
  quick_screen_current(ACTION_STD_QUICKSCREEN,NULL);assert(brightness==1&&applied==1&&saves==1);
  reset();brightness=1;
  add(BUTTON_PLAY|BUTTON_REPEAT,0,0,0);add(BUTTON_PLAY,0,0,0);
  add(BUTTON_RC_MENU,0,0,0);add(BUTTON_RC_MENU|BUTTON_REL,0,0,0);
  quick_screen_current(ACTION_STD_QUICKSCREEN,NULL);assert(brightness==63&&applied==63);
 }
 source_cider=true;reset();brightness=32;saves=tweaks=0;kind=0;
 add(BUTTON_LEFT,1,0,0);add(BUTTON_RIGHT,1,1,0);
 add(BUTTON_RC_MENU,1,1,0);add(BUTTON_RC_MENU|BUTTON_REL,1,1,0);
 quick_screen_current(ACTION_STD_QUICKSCREEN,NULL);assert(!saves&&!tweaks&&brightness==32);
 puts("FU34 local/Cider brightness: body MENU/PLAY, remote Up/Down, native step/callback/limits/persistence, native TOP/BOTTOM; remote Menu/Play and body Select exit PASS");

 puts("FU32 production key worker + quickscreen loop + native Cider values: device/remote arrows, wheel, physical MENU TOP, remote MENU/centre exit, Play-hold context PASS");
}
'''
(O/'test.c').write_text(pre+post);(O/'config.h').write_text('')
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-missing-field-initializers','-Wno-pointer-sign','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
