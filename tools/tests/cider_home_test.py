"""FU45: production Home order load/save and options; transient availability.
No device I/O. Spoken/tagnavi providers have no rows in this fixture.
"""
from pathlib import Path
import re, subprocess, sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
s=(R/'apps-ipod/root_menu.c').read_text()
def fn(s,n):
 m=re.search(r'(?m)^(?:static[ \t]+)?(?:int|unsigned|bool|void|char|struct menu_table)[ \t*]+'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
assert 'root_menu_place_cider' not in s
assert re.search(r'MENUITEM_RETURNVALUE_DYNTEXT\(cider_item, GO_TO_CIDER, item_callback,.*?cider_item_name, NULL, NULL, Icon_Playlist\)',s,re.S)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define HAVE_USB_PODLINK
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define MENU_COUNT_SHIFT 8
#define MENU_COUNT_MASK 255
#define MENU_GET_COUNT(f) (((f)>>8)&255)
#define MENU_ITEM_COUNT(n) ((n)<<8)
#define MENU_HAS_DESC 1
#define MT_MENU 2
#define TAGNAVI_MAIN_MENU_SLOTS 1
struct menu_item_ex{unsigned int flags;const struct menu_item_ex**submenus;const void*callback_and_desc;};
static struct menu_item_ex root_menu_,db_browser,cider_item,shortcut_menu,menu_,podcasts_item,calendars_item,contacts_item,other,tag;
static int root_menu_desc;
struct menu_table{const char*string;const struct menu_item_ex*item;};
static struct menu_table menu_table[]={
 {"database",&db_browser},{"cider",&cider_item},{"shortcuts",&shortcut_menu},
 {"settings",&menu_},{"podcasts",&podcasts_item},{"other",&other},{"tagnavi0",&tag}};
#define MAX_MENU_ITEMS ARRAYLEN(menu_table)
static struct menu_item_ex*root_menu__[MAX_MENU_ITEMS];
static struct {int8_t idx;uint8_t after;}root_menu_off[MAX_MENU_ITEMS];
static unsigned root_menu_off_count;
static bool available;
static bool cider_available(void){return available;}
static int browser_db_get_main_menu_tag_row_count(void){return 0;}
static bool browser_db_get_main_menu_row(int n,void*p){(void)n;(void)p;return false;}
static bool browser_db_ready(void){return false;}
static void root_menu_fixup_tagnavi_slots(void){}
static void root_menu_set_default(void*s,void*v){(void)s;(void)v;assert(0);}
static char*skip_whitespace(char*s){while(*s==' ')s++;return s;}
static struct menu_item_ex*audiobooks_item(void){return NULL;}
static bool auto_absent(const struct menu_item_ex*i,unsigned n){(void)i;(void)n;return false;}
static void write_shown(char**p,int*n,const struct menu_item_ex*i){(void)p;(void)n;(void)i;assert(0);}
static const struct {const struct menu_item_ex*item;const struct menu_item_ex*after[3];}root_menu_homes[]={{&other,{&db_browser,NULL}}};
'''
functions='\n'.join(fn(s,n) for n in ['root_menu_refresh','root_menu_set_count','root_menu_active_count','root_menu_off_shift','root_menu_off_find','root_menu_find','root_menu_new_on','root_menu_place','root_menu_place_unlisted','root_menu_get_options','menu_table_index_of_key','root_menu_load_from_cfg','write_key','root_menu_write_to_cfg'])
post=r'''
static void load(const char*text){char cfg[512];bool customized=false;snprintf(cfg,sizeof cfg,"%s",text);root_menu_load_from_cfg(&customized,cfg);assert(customized);}
static void save(char*out){memset(out,0,512);root_menu_write_to_cfg(NULL,out,512);}
int main(void){
 int n;struct menu_table*options=root_menu_get_options(&n);int found=0;
 for(int i=0;i<n;i++)found+=options[i].item==&cider_item;assert(found==1);
 unsigned cases=0;
 const char*orders[]={"cider, database, other, settings", "database, other, cider, settings", "database, !cider, other, settings", "database, other, settings"};
 for(unsigned c=0;c<ARRAYLEN(orders);c++){
  load(orders[c]);char before[512],after[512];save(before);
  unsigned count=MENU_GET_COUNT(root_menu_.flags),off=root_menu_off_count;
  struct menu_item_ex*saved[MAX_MENU_ITEMS];memcpy(saved,root_menu__,sizeof saved);
  for(int cycle=0;cycle<20;cycle++){
   available=cycle%2;root_menu_refresh();assert(!root_menu_refresh());
   assert(MENU_GET_COUNT(root_menu_.flags)==count&&root_menu_off_count==off);
   assert(!memcmp(saved,root_menu__,count*sizeof(saved[0])));save(after);assert(!strcmp(before,after));cases++;
  }
  load(before);save(after);assert(!strcmp(before,after));
  if(c==0)assert(root_menu__[0]==&cider_item);
  if(c==1)assert(root_menu__[2]==&cider_item);
  if(c>=2)assert(root_menu_find(&cider_item,count)==count&&root_menu_off_find(&cider_item)>=0);
 }
 /* Moving or hiding is the same saved order consumed by Customize Main Menu. */
 load("database, cider, other, settings");load("other, database, cider, settings");
 assert(root_menu__[2]==&cider_item);load("other, database, !cider, settings");
 assert(root_menu_off_find(&cider_item)>=0);load("other, database, cider, settings");
 assert(root_menu__[2]==&cider_item);
 printf("FU45 Home: %u availability/order cases; moved/hidden/unlisted Cider survives save/reload; customization offers Cider PASS\n",cases);
}
'''
(O/'home.c').write_text(pre+functions+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-function','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(O/'home.c'),'-o',str(O/'home')],check=True)
subprocess.run([str(O/'home')],check=True)
