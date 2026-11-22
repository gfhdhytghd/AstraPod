"""FU45: body/remote Customize Main Menu move/hide/save/reload.
Production configuration controller and root order parser; UI/I/O adapters only.
"""
from pathlib import Path
import re, subprocess, sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
adapter=R/'tools/tests/cider_home_test.py';ns={'__file__':str(adapter),'__name__':'fu45_home_adapters'}
exec(compile(adapter.read_text().split("subprocess.run(['cc'")[0],str(adapter),'exec'),ns)
fn=ns['fn'];source=(O/'home.c').read_text();source=source.replace(fn(source,'main'),'')
c=(R/'apps-ipod/screens/main_menu_config.c').read_text()
k=(R/'apps-ipod/input/keymap_ipod.c').read_text();maps=[]
for name in ['button_context_standard','remote_button_context_standard']:
 t=re.search(r'static const struct button_mapping '+name+r'\[\].*?\n};',k,re.S).group()
 maps+=re.findall(r'\{\s*ACTION_STD_(?:OK|CANCEL|MENU),[^}]+\}',t)
extra=r'''
#define IPOD_ACCESSORY_PROTOCOL
#include "firmware/target/arm/ipod/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define HZ 100
#define ID2P(x) (x)
#define FOR_NB_SCREENS(i) for(int i=0;i<1;i++)
#define MENUITEM_STRINGLIST(name,...) int name=0
#define CONTEXT_LIST 0
#define SYS_USB_CONNECTED -100
#define MAX_ITEMS 7
#define MAX_ITEM_NAME 64
#define CFG_STR_SIZE 512
#define LANG_FAILED 1
#define LANG_ON 2
#define LANG_OFF 3
#define LANG_MAIN_MENU_SETTINGS 4
#define Icon_Menu_setting 5
#define LANG_LOAD_DEFAULT_CONFIGURATION 6
#define LANG_MOVE 7
enum{ACTION_STD_OK=100,ACTION_STD_CANCEL,ACTION_STD_MENU,ACTION_STD_CONTEXT};
static struct{bool root_menu_customized,show_icons,talk_menu;}global_settings;
struct items{unsigned char*name;char dyn_name[64],string[64];bool enabled;};
static struct items menu_items[MAX_ITEMS];
static struct menu_table*custom_table;
static int menu_item_count,moving_item=-1,moving_from=-1,remote_keys,test_case,step,saves;
struct gui_synclist{int selected;};
struct key_entry{int action,button,previous;};
static const struct key_entry keys[]={KEYS};
static int key(int body,int remote){int b=remote_keys?remote:body;
 for(unsigned i=0;i<ARRAYLEN(keys);i++)if(keys[i].button==(b|BUTTON_REL)&&keys[i].previous==b)return keys[i].action;
 assert(0);return 0;}
static unsigned char*item_name(int i,struct items*dest){(void)dest;return (unsigned char*)custom_table[i].string;}
static int menu_get_name,menu_speak_item,menu_get_icon,menu_get_kind;
static int str(int n){return n;}
static void settings_save(void){saves++;}
static void splash(int n,int s){(void)n;(void)s;assert(0);}
static void talk_id(int n,bool q){(void)n;(void)q;}
static void viewportmanager_theme_enable(int n,bool b,void*p){(void)n;(void)b;(void)p;}
static void viewportmanager_theme_undo(int n,bool b){(void)n;(void)b;}
static void gui_synclist_init(struct gui_synclist*l,int n,void*p,bool b,int q,void*v){(void)n;(void)p;(void)b;(void)q;(void)v;l->selected=0;}
static void gui_synclist_set_voice_callback(struct gui_synclist*l,int n){(void)l;(void)n;}
static void gui_synclist_set_icon_callback(struct gui_synclist*l,int n){(void)l;(void)n;}
static void gui_synclist_set_row_callbacks(struct gui_synclist*l,int n,void*a,void*b){(void)l;(void)n;(void)a;(void)b;}
static void gui_synclist_set_nb_items(struct gui_synclist*l,int n){(void)l;(void)n;}
static void gui_synclist_select_item(struct gui_synclist*l,int n){l->selected=n;}
static int gui_synclist_get_sel_pos(struct gui_synclist*l){return l->selected;}
static void gui_synclist_set_title(struct gui_synclist*l,int n,int i){(void)l;(void)n;(void)i;}
static void gui_synclist_draw_settled(struct gui_synclist*l){(void)l;}
static void gui_synclist_draw(struct gui_synclist*l){(void)l;}
static void gui_synclist_speak_item(struct gui_synclist*l){(void)l;}
static int default_event_handler(int a){(void)a;assert(0);return 0;}
static bool yesno_pop_confirm(int n){(void)n;assert(0);return false;}
static int do_menu(int*m,void*a,void*b,bool c){(void)m;(void)a;(void)b;(void)c;assert(0);return 0;}
static bool list_do_action(int c,int timeout,struct gui_synclist*l,int*a){
 (void)c;(void)timeout;assert(step<4);
 if(test_case==0){
  if(step++==0){assert(!strcmp(menu_items[l->selected].string,"cider"));l->selected=0;return true;}
  *a=step==2?key(BUTTON_SELECT,BUTTON_RC_SELECT):key(BUTTON_MENU,BUTTON_RC_MENU);return false;
 }
 /* Start one row before Cider; selecting it is modeled list scrolling. */
 if(step++==0){for(int i=0;i<menu_item_count;i++)if(!strcmp(menu_items[i].string,"cider"))l->selected=i;return true;}
 *a=step==2?key(BUTTON_SELECT,BUTTON_RC_SELECT):key(BUTTON_MENU,BUTTON_RC_MENU);return false;
}
'''.replace('KEYS',','.join(maps))
functions='\n'.join(fn(c,n) for n in ['load_from_cfg','save_to_cfg','swap_items','move_item','item_is_locked','find_item','config_screen'])
functions=functions.replace('menu_table[','custom_table[').replace('menu_table =','custom_table =')
post=r'''
int main(void){
 for(remote_keys=0;remote_keys<2;remote_keys++){
  load("database, other, cider, settings");step=saves=0;test_case=0;
  assert(config_screen(&cider_item)==0&&saves==1&&root_menu__[0]==&cider_item);
  char saved[512];save(saved);load(saved);assert(root_menu__[0]==&cider_item);
  step=saves=0;test_case=1;assert(config_screen(NULL)==0&&saves==1);
  assert(root_menu_off_find(&cider_item)>=0&&root_menu_find(&cider_item,MENU_GET_COUNT(root_menu_.flags))==MENU_GET_COUNT(root_menu_.flags));
  save(saved);load(saved);available=false;root_menu_refresh();available=true;root_menu_refresh();assert(root_menu_off_find(&cider_item)>=0);
  step=saves=0;assert(config_screen(NULL)==0&&saves==1&&root_menu__[0]==&cider_item);
 }
 puts("FU45 Home body/remote: carry Cider -> move/drop -> save/reload -> hide -> availability cycle -> unhide at saved position PASS");
}
'''
(O/'config.h').write_text('')
(O/'home-keys.c').write_text(source+extra+functions+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(R),'-I'+str(O),str(O/'home-keys.c'),'-o',str(O/'home-keys')],check=True)
subprocess.run([str(O/'home-keys')],check=True,timeout=30)
