"""FU32 production menu/simplelist loops and Cider callbacks, recorded key paths.
Drawing, settings-row editing and USB are deterministic adapters; no device I/O.
"""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
def fn(s,n):
 m=re.search(r'(?m)^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
a=(R/'apps-ipod/input/action.c').read_text();menu=(R/'apps-ipod/widgets/menu.c').read_text();listing=(R/'apps-ipod/widgets/list.c').read_text();controls=re.sub(r'^#include .*\n','',(R/'apps-ipod/screens/playback/cider_controls.c').read_text(),flags=re.M)
loop=fn(menu,'do_menu');start=loop.index('        else if (action == ACTION_STD_CONTEXT)');end=loop.index('        else if (action == ACTION_STD_MENU)',start);loop=loop[:start]+'        else if (action == ACTION_STD_CONTEXT) {}\n'+loop[end:]
k=(R/'apps-ipod/input/keymap_ipod.c').read_text();tables='\n'.join(re.search(r'static const struct button_mapping '+name+r'\[\].*?\n};',k,re.S).group() for name in ['button_context_standard','remote_button_context_standard'])
tokens=sorted({"LANG_SLEEP_TIMER","ACTION_UNKNOWN"}|set(re.findall(r'\b(?:ACTION_|LANG_|GO_TO_|CONTEXT_|ACTIVITY_|Icon_)\w+',controls+loop+fn(listing,'simplelist_show_list')+tables)))
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "screens/playback/cider_model.h"
#include "screens/playback/cider_store.h"
#include "screens/playback/cider_library.h"
#define IPOD_ACCESSORY_PROTOCOL
#include "../firmware/target/arm/s5l8702/ipod6g/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define HZ 100
#define SYS_USB_CONNECTED -100
#define SYS_CALL_HUNG_UP -101
#define BUTTON_MULTIMEDIA_PLAYPAUSE -102
#define TIMEOUT_NOBLOCK 0
#define ALLOW_SOFTLOCK 0
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for(int i=0;i<1;i++)
#define ARRAYLEN(x) (sizeof(x)/sizeof((x)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define has_flag(a,b) (((a)&(b))==(b))
#define LAST_ITEM_IN_LIST {0,0,0}
#define LAST_ITEM_IN_LIST__NEXTLIST(c) {0,0,0}
#define MENU_TYPE_MASK 15
#define MT_MENU 1
#define MT_FUNCTION_CALL 2
#define MT_FUNCTION_CALL_W_PARAM 3
#define MT_RETURN_ID 4
#define MT_RETURN_VALUE 5
#define MT_SETTING 6
#define MT_SETTING_W_TEXT 7
#define MENU_EXITAFTERTHISMENU 16
#define MENU_FUNC_CHECK_RETVAL 32
#define MENU_SELECTED_EXIT -2
#define MENU_ATTACHED_USB -3
#define MAX_MENUS 8
#define QUICKSCREEN_GOTO_SHORTCUTS_MENU 2
static int quick_screen_current(int action,void(*poll)(void)){(void)action;(void)poll;return 0;}
#define MENU_GET_COUNT(f) ((unsigned)(f)>>8)
struct viewport{int x;};static struct viewport vp;
struct gui_synclist{int selected_item,count,title_icon;const char*title;struct viewport*parent[1];};
typedef const char*list_get_name(int,void*,char*,size_t);
struct simplelist_info{const char*title;int count,selection,timeout,title_icon,selection_size,top_item,row_height;void*callback_data,*get_icon,*get_talk,*get_color,*get_kind,*get_value,*get_range,*get_albumart,*selection_color;bool hide_theme,scroll_all,speak_onshow,wps_on_play;list_get_name*get_name;int(*action_callback)(int,struct gui_synclist*);};
struct menu_function{int(*function)(void);};struct menu_param{int(*function_w_param)(void*);void*param;};
struct menu_item_ex{int flags,title,value;const struct menu_item_ex**submenus;const struct menu_function*function;const struct menu_param*function_param;};
struct menu_data_t{const struct menu_item_ex*menu;int selected;};
typedef int(*menu_callback_type)(int,const struct menu_item_ex*,struct gui_synclist*);
enum list_row_kind{LIST_ROW_ACTION,LIST_ROW_SETTING,LIST_ROW_TOGGLE};struct menu_row_presenter{bool(*describe)(const struct menu_item_ex*,enum list_row_kind*,const char**);};
static const struct menu_row_presenter*presenter;static const struct menu_row_presenter*menu_set_row_presenter(const struct menu_row_presenter*p){const struct menu_row_presenter*old=presenter;presenter=p;return old;}
#define MENUITEM_FUNCTION(n,f,t,fn,cb,icon) static const struct menu_function n##_fn={fn};static const struct menu_item_ex n={.flags=MT_FUNCTION_CALL|(f),.title=(t),.function=&n##_fn}
#define MENUITEM_FUNCTION_W_PARAM(n,f,t,fn,p,cb,icon) static const struct menu_param n##_fn={fn,p};static const struct menu_item_ex n={.flags=MT_FUNCTION_CALL_W_PARAM|(f),.title=(t),.function_param=&n##_fn}
#define MENUITEM_FUNCTION_DYNTEXT_W_PARAM(n,f,fn,p,text,voice,data,cb,icon) MENUITEM_FUNCTION_W_PARAM(n,f,0,fn,p,cb,icon)
#define MENUITEM_FUNCTION_DYNTEXT(n,f,fn,text,voice,data,cb,icon) MENUITEM_FUNCTION(n,f,0,fn,cb,icon)
#define MAKE_MENU(n,t,cb,icon,...) static const struct menu_item_ex*n##_rows[]={__VA_ARGS__};static const struct menu_item_ex n={.flags=MT_MENU|(ARRAYLEN(n##_rows)<<8),.title=(t),.submenus=n##_rows}
static char labels[512][24];static const char*str(int id){assert(id>=0&&id<512);return labels[id];}
#define ID2P(x) (x)
static struct {bool talk_menu,shortcuts_replaces_qs,show_icons;}global_settings;
static struct{int last_screen;}global_status;
static int current_subitems_count;static const struct menu_item_ex*current_submenus_menu;
static const struct menu_item_ex root_menu_={0};
static int empty_menu_callback(int a,const struct menu_item_ex*m,struct gui_synclist*l){(void)m;(void)l;return a;}
static void get_menu_callback(const struct menu_item_ex*m,menu_callback_type*cb){(void)m;*cb=empty_menu_callback;}
static int get_menu_selection(int n,const struct menu_item_ex*m){(void)m;return n;}
static char*init_title(const struct menu_item_ex*m,int*i,char*b,size_t n){(void)i;(void)b;(void)n;return (char*)str(m->title);}
static struct gui_synclist*active_list;static int activity;
static int get_current_activity(void){return activity;}static void push_current_activity(int a){activity=a;}static void pop_current_activity(void){activity=0;}
static void gui_synclist_select_item(struct gui_synclist*l,int n){l->selected_item=n;}
static int gui_synclist_get_sel_pos(struct gui_synclist*l){return l->selected_item;}
static void gui_synclist_draw_settled(struct gui_synclist*l){active_list=l;}static void gui_synclist_draw(struct gui_synclist*l){active_list=l;}
static void gui_synclist_speak_item(struct gui_synclist*l){(void)l;}static void gui_synclist_scroll_stop(struct gui_synclist*l){(void)l;}
static void gui_synclist_set_title(struct gui_synclist*l,const char*t,int i){l->title=t;l->title_icon=i;}
static void gui_synclist_set_nb_items(struct gui_synclist*l,int n){l->count=n;}
static void gui_synclist_init(struct gui_synclist*l,list_get_name*f,void*d,bool b,int n,void*p){(void)f;(void)d;(void)b;(void)n;(void)p;memset(l,0,sizeof*l);l->parent[0]=&vp;}
#define gui_synclist_set_icon_callback(...) ((void)0)
#define gui_synclist_set_voice_callback(...) ((void)0)
#define gui_synclist_set_color_callback(...) ((void)0)
#define gui_synclist_set_row_callbacks(...) ((void)0)
#define gui_synclist_set_range_callback(...) ((void)0)
#define gui_synclist_set_albumart_callback(...) ((void)0)
#define gui_synclist_set_row_height(...) ((void)0)
#define gui_synclist_set_sel_color(...) ((void)0)
#define gui_synclist_set_top_item(...) ((void)0)
#define keyclick_set_callback(...) ((void)0)
#define viewportmanager_theme_enable(...) ((void)0)
#define viewportmanager_theme_undo(...) ((void)0)
#define sb_set_persistent_title(...) ((void)0)
#define skinlist_set_cfg(...) ((void)0)
static int audio_status(void){return 1;}static bool query_audio_status(int*p){(void)p;return false;}static bool root_menu_refresh(void){return false;}
static int list_do_action_timeout(struct gui_synclist*l,int n){(void)l;return n;}
static void gui_synclist_forget(struct gui_synclist*l){(void)l;}
static int do_shortcut_menu(void*p){(void)p;return 0;}static bool list_stop_handler(void){return false;}
static void do_setting_from_menu(const struct menu_item_ex*m,struct viewport*p){(void)m;(void)p;}
static int default_event_handler(int a){(void)a;return 0;}static bool list_is_dirty(struct gui_synclist*l){(void)l;return false;}
static void talk_shutup(void){}static void yield(void){}
static int simplelist_line_count;static const char*simplelist_static_getname(int n,void*d,char*b,size_t z){(void)n;(void)d;(void)b;(void)z;return "row";}
static void simplelist_info_init(struct simplelist_info*i,const char*t,int n,void*d){memset(i,0,sizeof*i);i->title=t;i->count=n;i->callback_data=d;i->selection_size=1;i->top_item=-1;i->timeout=10;}
static void simplelist_reset_lines(void){simplelist_line_count=0;}static void simplelist_addline(const char*f,...){(void)f;simplelist_line_count++;}
static unsigned long current_tick=100;static int modes[2],control_calls;static char route[160];
static int cider_mode_get(int m){return modes[m];}static void cider_mode_set(int m,int v){modes[m]=v;}
static void cider_control(const char*c,int v){(void)c;(void)v;control_calls++;}
static void cider_page_open(const char*q){snprintf(route,sizeof route,"%s",q);}static int cider_page_total(void){return !strncmp(route,"now/info",8)?12:1;}
static bool cider_page_row(int n,struct cider_row*r){memset(r,0,sizeof*r);snprintf(r->id,sizeof r->id,"title");snprintf(r->title,sizeof r->title,"value");snprintf(r->route,sizeof r->route,"album/a");r->count=0;return n<12;}
static void cider_store_stats(struct cider_store_stats*s){memset(s,0,sizeof*s);}static void cider_store_build(bool b){(void)b;}static bool cider_store_building(void){return false;}
static unsigned cider_read_ahead_count(unsigned a,unsigned b){(void)a;(void)b;return 6;}static int cider_wps_volume(void*p){(void)p;return 37;}
__attribute__((unused)) static void splash(int n,const char*t){(void)n;(void)t;}static void splashf(int n,const char*f,...){(void)n;(void)f;}static const char*cider_remote_name(void){return "Cider";}static void action_wait_for_release(void){}
static int get_action(int c,int t);static int do_menu(const struct menu_item_ex*,int*,struct viewport[NB_SCREENS],bool);
static bool simplelist_show_list(struct simplelist_info*);
static int lyric_viewer(void){struct gui_synclist lyrics={.title=str(LANG_LYRICS)};active_list=&lyrics;int action;do{action=get_action(0,1);}while(action==ACTION_NONE);assert(action==ACTION_STD_CANCEL);return 0;}
static int view_text(const char*t,const char*v){(void)t;(void)v;int action;do{action=get_action(0,1);}while(action==ACTION_NONE);assert(action==ACTION_STD_CANCEL);return 0;}
static bool cider_wps_show_art(void){int action;do{action=get_action(0,1);}while(action==ACTION_NONE);assert(action==ACTION_STD_CANCEL);return false;}
int cider_browse_at(const struct cider_row*r){(void)r;int action;do{action=get_action(0,1);}while(action==ACTION_NONE);assert(action==ACTION_STD_CANCEL);return GO_TO_PREVIOUS;}
static int sleep_page(void){struct simplelist_info i;simplelist_info_init(&i,str(LANG_SLEEP_TIMER),3,NULL);return simplelist_show_list(&i);}
int cider_native_sleep_timer(void){return sleep_page();}char*cider_native_sleep_name(int a,void*b,char*c,size_t d){(void)a;(void)b;(void)c;(void)d;return (char*)str(LANG_SLEEP_TIMER);}
static const struct menu_item_ex theme_options_item={.flags=MT_FUNCTION_CALL,.function=&(const struct menu_function){sleep_page}};
static int init_menu_lists(const struct menu_item_ex*m,struct gui_synclist*l,int n,bool cb,struct viewport*p,char*b,size_t z){(void)cb;(void)p;(void)b;(void)z;memset(l,0,sizeof*l);l->parent[0]=&vp;l->selected_item=n;l->count=MENU_GET_COUNT(m->flags);l->title=str(m->title);current_subitems_count=l->count;current_submenus_menu=m;active_list=l;return ACTION_ENTER_MENUITEM;}
'''
# Functions below reference action/language enums; insert those before the adapter.
pre=pre.replace('#include "../firmware/target/arm/s5l8702/ipod6g/button-target.h"','#include "firmware/target/arm/s5l8702/ipod6g/button-target.h"')
pos=pre.index('struct viewport');pre=pre[:pos]+'enum {'+','.join(tokens)+'};\n'+pre[pos:]
pre+='\nstruct button_mapping{int action_code,button_code,pre_button_code;};typedef struct{int button;}action_last_t;typedef struct{int button;bool is_prebutton;const struct button_mapping*items;}action_cur_t;\n'+tables+fn(a,'action_code_worker')+'\nstatic bool menu_back;\n'+fn(a,'action_set_menu_back')+fn(a,'action_menu_back')+fn(a,'menu_back_action')
pre+=r'''
struct event{int key,title,row;};static struct event events[256];static int used,at,prev;
static bool gui_synclist_do_button(struct gui_synclist*l,int*a){if(*a==ACTION_STD_NEXT||*a==ACTION_STD_PREV){l->selected_item=MAX(0,MIN(l->count-1,l->selected_item+(*a==ACTION_STD_NEXT?1:-1)));active_list=l;return true;}return false;}
static int get_action(int c,int t){(void)c;(void)t;assert(at<used);struct event*e=&events[at++];if(active_list->title!=str(e->title)||active_list->selected_item!=e->row){fprintf(stderr,"event %d key %x actual %s/%d expected %s/%d\n",at,e->key,active_list->title,active_list->selected_item,str(e->title),e->row);abort();}
 action_last_t l={prev};action_cur_t v={.button=e->key,.items=e->key&BUTTON_REMOTE?remote_button_context_standard:button_context_standard};int n=0;int a=action_code_worker(&l,&v,&n);prev=e->key;return menu_back_action(a==ACTION_UNKNOWN?ACTION_NONE:a);}
static void list_do_action(int c,int t,struct gui_synclist*l,int*a){active_list=l;*a=get_action(c,t);gui_synclist_do_button(l,a);if(*a==ACTION_STD_NEXT||*a==ACTION_STD_PREV)*a=ACTION_NONE;}
'''
post=r'''
static void event(int key,int title,int row){events[used++]=(struct event){key,title,row};}
static void tap(int key,int title,int row){event(key,title,row);event(key|BUTTON_REL,title,row);}
static void run(int target,int native_flags,bool remote_key){used=at=prev=0;modes[0]=modes[1]=0;int select=remote_key?BUTTON_RC_SELECT:BUTTON_SELECT,back=remote_key?BUTTON_RC_MENU:BUTTON_MENU;
 for(int i=0;i<target;i++)event(BUTTON_SCROLL_FWD,LANG_NOW_PLAYING,i);
 tap(select,LANG_NOW_PLAYING,target);
 /* The live leaf's simplelist, browser or art loop reads this MENU. */
 if(target==0||target==1){int title=target?LANG_REPEAT:LANG_SHUFFLE;event(BUTTON_SCROLL_FWD,title,0);tap(select,title,1);}
 else if(target==2)tap(select,LANG_MENU_SET_RATING,0);
 else if(target==3){tap(select,LANG_TRACK_INFO,0);tap(back,LANG_TRACK_INFO,0);tap(back,LANG_TRACK_INFO,0);}
 else if(target==4||target==5)tap(back,LANG_NOW_PLAYING,target);
 else if(target==6)tap(back,LANG_LYRICS,0);
 else if(target==7||target==10)tap(select,LANG_SLEEP_TIMER,0);
 else if(target==8){event(BUTTON_SCROLL_FWD,LANG_SPEED,2);tap(select,LANG_SPEED,3);}
 else if(target==9){tap(select,LANG_CURRENT_PLAYLIST,0);tap(select,LANG_CURRENT_PLAYLIST,0);tap(back,LANG_CURRENT_PLAYLIST,0);tap(back,LANG_CURRENT_PLAYLIST,0);}
 else if(target==11){tap(select,LANG_MORE,0);tap(back,LANG_MORE,0);event(BUTTON_SCROLL_FWD,LANG_MORE,0);tap(select,LANG_MORE,1);tap(select,LANG_SOUND_SETTINGS,0);event(BUTTON_SCROLL_BACK,LANG_VOLUME,0);tap(back,LANG_VOLUME,0);tap(back,LANG_SOUND_SETTINGS,0);tap(back,LANG_MORE,1);}
 
 if(target!=6)tap(back,LANG_NOW_PLAYING,target);
 if(native_flags){struct menu_item_ex native=cider_context_menu;native.flags|=MENU_EXITAFTERTHISMENU;action_set_menu_back(true);int selected=0;do_menu(&native,&selected,NULL,false);action_set_menu_back(false);}
 else {assert(!cider_context_show());}
 assert(at==used&&!menu_back&&!presenter);
}
int main(void){for(unsigned i=0;i<ARRAYLEN(labels);i++)snprintf(labels[i],sizeof labels[i],"label-%u",i);
 for(int flags=0;flags<2;flags++)for(int remote=0;remote<2;remote++)for(int row=0;row<12;row++)run(row,flags,remote);
 puts("FU32 production action worker + do_menu + simplelist loops: device/remote MENU returns each leaf; lyrics exits directly to WPS PASS");}
'''
(O/'config.h').write_text('')
(O/'test.c').write_text(pre+fn(listing,'simplelist_show_list')+loop+controls+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-unused-but-set-variable','-Wno-pointer-sign','-Wno-enum-conversion','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),str(O/'test.c'),'-o',str(O/'test')],check=True)
subprocess.run([str(O/'test')],check=True)
