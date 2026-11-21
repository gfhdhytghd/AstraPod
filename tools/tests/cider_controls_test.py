"""FU30 native menu/widget binding and real control callbacks, offline only."""
from pathlib import Path
import re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
s=(r/'apps-ipod/screens/playback/cider_controls.c').read_text();s=re.sub(r'^#include .*\n','',s,flags=re.M)
tokens=sorted(set(re.findall(r'\b(?:LANG_|ACTION_|CONTEXT_|ACTIVITY_|GO_TO_|Icon_)\w+',s)))
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "screens/playback/cider_model.h"
#include "screens/playback/cider_store.h"
#include "screens/playback/cider_library.h"
struct ui_scene { int unused; };
static void ui_scene_open_page(struct ui_scene *s,const char *route){(void)s;assert(route);}
static void ui_scene_open_modal(struct ui_scene *s,const char *route){(void)s;assert(route);}
static bool ui_scene_close_modal(struct ui_scene *s,bool animate,const char *route)
{(void)s;(void)animate;(void)route;return false;}

#define HZ 100
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)
#define SYS_USB_CONNECTED -100
#define ID2P(x) (x)
#define MENU_FUNC_CHECK_RETVAL 0
struct gui_synclist {int selected;};
enum list_row_kind {LIST_ROW_ACTION,LIST_ROW_SETTING,LIST_ROW_TOGGLE};
struct menu_item_ex {int title;const struct menu_item_ex **items;unsigned count;};
struct menu_row_presenter {bool(*describe)(const struct menu_item_ex*,enum list_row_kind*,const char**);};
static const struct menu_row_presenter *presenter;
static const struct menu_row_presenter*menu_set_row_presenter(const struct menu_row_presenter*p){const struct menu_row_presenter*old=presenter;presenter=p;return old;}
#define MENUITEM_FUNCTION(name,flags,title,fn,cb,icon) static const struct menu_item_ex name={title,0,0}
#define MENUITEM_FUNCTION_W_PARAM(name,flags,title,fn,param,cb,icon) MENUITEM_FUNCTION(name,flags,title,fn,cb,icon)
#define MENUITEM_FUNCTION_DYNTEXT_W_PARAM(name,flags,fn,param,text,voice,data,cb,icon) MENUITEM_FUNCTION(name,flags,0,fn,cb,icon)
#define MENUITEM_FUNCTION_DYNTEXT(name,flags,fn,text,voice,data,cb,icon) MENUITEM_FUNCTION(name,flags,0,fn,cb,icon)
#define MAKE_MENU(name,title,cb,icon,...) static const struct menu_item_ex *name##_rows[]={__VA_ARGS__};static const struct menu_item_ex name={title,name##_rows,ARRAYLEN(name##_rows)}
static const struct menu_item_ex theme_options_item={0,0,0};
struct simplelist_info {const char*title;int count,selection,timeout;void*callback_data;const char*(*get_name)(int,void*,char*,size_t);int(*action_callback)(int,struct gui_synclist*);};
static unsigned long current_tick=100;
static int phase,choose,shown_count,shown_selection,mode_values[2],sets,controls,pushes,pops,sleeps,browse_calls;
static int volume_steps;static char command[64],route[160],lines[10][64];static int line_count;
static bool favorite,in_library,building;
static const char *str(int id){static char b[32];snprintf(b,sizeof b,"lang%d",id);return b;}
static void simplelist_info_init(struct simplelist_info*i,const char*t,int n,void*d){memset(i,0,sizeof*i);i->title=t;i->count=n;i->callback_data=d;}
static void simplelist_reset_lines(void){line_count=0;}
static void simplelist_addline(const char*f,...){va_list a;va_start(a,f);vsnprintf(lines[line_count++],64,f,a);va_end(a);}
static bool simplelist_show_list(struct simplelist_info*i){shown_count=i->count;shown_selection=i->selection;if(phase==1||phase==2)i->selection=choose;return false;}
static int gui_synclist_get_sel_pos(struct gui_synclist*l){return l->selected;}
static int cider_mode_get(int m){return mode_values[m];}
static void cider_mode_set(int m,int v){mode_values[m]=v;sets++;}
static void cider_control(const char*c,int v){controls++;snprintf(command,sizeof command,"%s",c);volume_steps+=v;}
static void cider_page_open(const char*q){snprintf(route,sizeof route,"%s",q);}
static int cider_page_total(void){return !strncmp(route,"now/info",8)?12:!strncmp(route,"now/queue",9)?60:1;}
static bool cider_page_row(int index,struct cider_row*row){static const char*ids[]={"title","artist","album","disc","track","year","genre","duration","quality","favorite","library","speed"};static const char*values[]={"Title","Artist","Album","2","3","2020","Rock","123000","AAC 256kbps","0","0","1.25"};memset(row,0,sizeof*row);if(!strncmp(route,"now/info",8)){if(index>=12)return false;snprintf(row->id,sizeof row->id,"%s",ids[index]);snprintf(row->title,sizeof row->title,"%s",values[index]);if(index==9)snprintf(row->title,sizeof row->title,"%d",favorite);if(index==10)snprintf(row->title,sizeof row->title,"%d",in_library);}else{row->count=55;strcpy(row->title,"Album");strcpy(row->route,"album/a1");}return true;}
static void cider_store_stats(struct cider_store_stats*s){memset(s,0,sizeof*s);}
static void cider_store_build(bool enable){building=enable;}
static bool cider_store_building(void){return building;}
static unsigned cider_read_ahead_count(unsigned speed,unsigned visible){(void)speed;(void)visible;return 6;}
static int cider_wps_volume(bool*m){(void)m;return 37;}
static bool cider_wps_show_art(void){return false;}
static int get_action(int c,int t){(void)c;current_tick+=t;return 0;}
static int default_event_handler(int a){(void)a;return 0;}
__attribute__((unused)) static void splash(int t,const char*s){(void)t;(void)s;}static void splashf(int n,const char*f,...){(void)n;(void)f;}static const char*cider_remote_name(void){return "Cider";}
static bool menu_back;static bool action_set_menu_back(bool on){bool old=menu_back;menu_back=on;return old;}
static void action_wait_for_release(void){}
static void push_current_activity(int a){(void)a;pushes++;}
static void pop_current_activity(void){pops++;}
static int view_text(const char*t,const char*v){(void)t;(void)v;return 0;}
int cider_native_sleep_timer(void){sleeps++;return 0;}
int lyric_viewer(void){return 0;}
char*cider_native_sleep_name(int i,void*d,char*b,size_t n){(void)i;(void)d;snprintf(b,n,"Sleep Timer");return b;}
int cider_browse_at(const struct cider_row*r){assert(!strcmp(r->route,"album/a1"));browse_calls++;return 0;}
static int do_menu(const struct menu_item_ex*m,void*a,void*b,bool c);
'''
post=r'''
static int do_menu(const struct menu_item_ex*m,void*a,void*b,bool c){(void)a;(void)b;(void)c;
 const struct menu_item_ex *expected[]={&cider_shuffle,&cider_repeat,&cider_rating_item,&cider_info_item,&cider_album_item,&cider_artist_item,&cider_lyrics_item,&cider_sleep_item,&cider_speed_item,&cider_queue_menu,&cider_more_menu};
 assert(m->count==ARRAYLEN(expected));for(unsigned i=0;i<ARRAYLEN(expected);i++)assert(m->items[i]==expected[i]);
 assert(cider_queue_menu.count==2&&cider_more_menu.count==2&&cider_sound_menu.count==1);
 assert(presenter==&context_presenter);enum list_row_kind kind;const char*value;
 assert(presenter->describe(&cider_shuffle,&kind,&value)&&kind==LIST_ROW_TOGGLE);
 assert(presenter->describe(&cider_repeat,&kind,&value)&&kind==LIST_ROW_SETTING);
 assert(!presenter->describe(&cider_info_item,&kind,&value));return 0;
}
int main(void){assert(!cider_context_show()&&pushes==pops&&!presenter);
 phase=1;for(int mode=0;mode<2;mode++)for(int i=0;i<(mode?5:2);i++){choose=i;assert(!mode_choose((void*)(intptr_t)mode));assert(mode_values[mode]==i&&shown_count==(mode?5:2));}
 phase=2;favorite=true;in_library=false;choose=0;rating();assert(!strcmp(command,"unlove")&&line_count==2);choose=1;rating();assert(!strcmp(command,"add-library"));favorite=false;in_library=true;choose=0;rating();assert(!strcmp(command,"love")&&line_count==1);
 phase=0;track_info();assert(shown_count==9);char b[200];assert(strstr(page_name(0,0,b,sizeof b),"Title"));assert(strstr(page_name(7,0,b,sizeof b),"2:03"));assert(strstr(page_name(8,0,b,sizeof b),"AAC 256kbps"));
 queue_view();assert(shown_count==60&&shown_selection==55);struct gui_synclist list={55};assert(page_action(ACTION_STD_OK,&list)==ACTION_REDRAW&&!strcmp(command,"queue-jump")&&volume_steps==55);
 browse((void*)0);browse((void*)1);assert(browse_calls==2);volume_action(ACTION_STD_PREV,&list);assert(!strcmp(command,"volume-step"));store_action(ACTION_STD_OK,&(struct gui_synclist){0});assert(building);
 puts("FU30 native menu order/widget values, five Repeat modes, love/unlove/library, native-style Track Info, current queue highlight/jump, navigation and cache build control PASS");}
'''
(o/'config.h').write_text('');(o/'test.c').write_text(pre+'enum {'+','.join(tokens)+'};\n'+s+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(r/'apps-ipod'),'-I'+str(o),str(o/'test.c'),'-o',str(o/'test')],check=True)
subprocess.run([str(o/'test')],check=True)
# Runtime menu declarations contain no local music operations.
root=s[s.index('MAKE_MENU(cider_context_menu'):s.index('static bool context_setting')]
for forbidden in ['delete','book','cue','chapter','mix','spike','save','search','reshuffle','equalizer','playback_settings']:
 assert forbidden not in root,forbidden
wps=(r/'apps-ipod/screens/playback/wps.c').read_text();assert 'cider_context_show()' in wps and 'cider_wps_update(state);' in wps
assert 'cider_wps_enter();' in (r/'apps-ipod/screens/playback/cider_wps.c').read_text()
