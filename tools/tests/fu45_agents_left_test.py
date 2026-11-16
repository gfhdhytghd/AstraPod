"""FU45: actual Agents dispatch/question handler/LEFT key rows/USB exit gate.
Drawing, transport and child transitions are deterministic call-recording adapters.
No hardware or service I/O. Check every production view for body/remote parity.
"""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
s=(R/'apps-ipod/screens/system/agents_screen.c').read_text();k=(R/'apps-ipod/input/keymap_ipod.c').read_text()
def fn(s,n):
 m=re.search(r'(?m)^(?:static[ \t]+)?(?:int|bool|void)[ \t*]+'+n+r'\([^;{}]*\)\s*\{',s);assert m,n
 e=m.end();d=1
 while d:d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[m.start():e]
run=fn(s,'agents_run')
init=re.search(r'    int action, ret = [^;]+;',run).group()
a=run.index('        /* Before question_action:');z=run.index('\n    /* A hold ends',a)
dispatch=run[a:z].rstrip();assert dispatch.endswith('}');dispatch=dispatch[:-1]
start=run[run.index('    if (!podlink_open_agents'):run.index('    bool resume')]
views=re.search(r'enum view \{[^}]+\};',s).group()
rows=[]
for name in ['button_context_agents','remote_button_context_agents','button_context_standard','remote_button_context_standard']:
 t=re.search(r'static const struct button_mapping '+name+r'\[\].*?\n};',k,re.S).group()
 rows+=re.findall(r'\{\s*ACTION_(?:AGENTS_LEFT_REPEAT|STD_CANCEL|STD_MENU),\s*BUTTON_(?:RC_)?(?:LEFT|MENU)[^}]+\}',t)
assert len(rows)==7
special=set(re.findall(r'\bACTION_\w+',dispatch+fn(s,'question_action')))-{'ACTION_REPEAT'}
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_USB_CONFIRM
#include "screens/system/agents_screen.h"
#include "screens/system/herdremote_model.h"
#include "firmware/target/arm/ipod/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define ACTION_REPEAT 1
#define HZ 100
#define SYS_USB_CONNECTED 9999
#define QUICKSCREEN_IN_USB 1
ACTIONS
VIEWS
struct gui_synclist{int dummy;};
static enum view view;
static struct hr_question q;
static bool have_question,detail_ready,question_closing,talk_held,native_play_pending,done,quick_usb;
static int raw,status,question_choice,backspaces,usage,tabs,closed_detail,closed_space,left_question,answers,enters,consents,finishes;
static unsigned long native_play_at,current_tick;
static char pending_req[8],focus_req[8],talk_req[8];
static int get_action_statuscode(int*b){if(b)*b=raw;return status;}
static const struct hr_question*current_question(void){return have_question?&q:NULL;}
static void send_key(const char*k){if(!strcmp(k,"bs"))backspaces++;else assert(!strcmp(k,"enter"));}
static void enter_answer(struct gui_synclist*l){(void)l;enters++;view=VIEW_QUESTION;}
static void answer_number(int n){assert(n==q.yes);answers++;}
static void close_input(bool submit){assert(!submit);}
static void leave_question(struct gui_synclist*l){(void)l;left_question++;}
static void start_talk(struct gui_synclist*l,bool remote){(void)l;(void)remote;assert(0);}
static void finish_talk(bool discard){assert(discard);finishes++;}
static void finish_native_play(void){assert(0);}
static int agents_quickscreen(struct gui_synclist*l,int a){(void)l;(void)a;return quick_usb?QUICKSCREEN_IN_USB:0;}
static void close_usage(struct gui_synclist*l){(void)l;assert(0);}
static void close_detail(struct gui_synclist*l){(void)l;closed_detail++;}
static void open_usage(struct gui_synclist*l){(void)l;usage++;view=VIEW_USAGE;}
static void switch_tab(struct gui_synclist*l,int dir){(void)l;assert(dir==-1);tabs++;}
static void close_space(struct gui_synclist*l){(void)l;closed_space++;}
static void activate(struct gui_synclist*l){(void)l;assert(0);}
static int default_event_handler(int a){return a==SYS_USB_CONNECTED?SYS_USB_CONNECTED:0;}
static void usb_confirm_reopen(void){consents++;}
static void splash(int n,const char*s){(void)n;(void)s;}
static int background_link;
static bool podlink_open_agents(int cb){(void)cb;return false;}
struct mapping{int action,button,previous;};
static const struct mapping keys[]={ROWS};
static int lookup(int key){for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++)if(keys[i].button==key)return keys[i].action;return ACTION_NONE;}
'''.replace('ACTIONS','enum {'+','.join(sorted(special))+'};').replace('VIEWS',views).replace('ROWS',','.join(rows))
code=pre+fn(s,'question_action')+fn(s,'agents_menu_exit')+fn(s,'agents_usb_exit')
code+='\nstatic int dispatch(int input,int button){struct gui_synclist list;'+init+'action=input;raw=button;status=button&BUTTON_REPEAT?ACTION_REPEAT:0;done=false;do{\n'+dispatch+'\n}while(false);if(done)agents_usb_exit(ret);return ret;}\n'
code+='static int failed_open(void){'+start+'assert(0);return -1;}\n'
post=r'''
struct result{int view,bs,usage,tabs,details,spaces,questions,answers,enters,choice;};
static void reset(int v){
 view=v;have_question=detail_ready=true;question_closing=talk_held=native_play_pending=quick_usb=false;
 pending_req[0]=focus_req[0]=talk_req[0]=0;question_choice=backspaces=usage=tabs=closed_detail=closed_space=left_question=answers=enters=consents=finishes=0;
 q=(struct hr_question){.yes=1,.no=2};
}
static struct result result(void){return(struct result){view,backspaces,usage,tabs,closed_detail,closed_space,left_question,answers,enters,question_choice};}
int main(void){
 int views=0,cases=0;
 for(int v=VIEW_SPACES;v<=VIEW_INPUT;v++){
  struct result body={0};
  for(int remote=0;remote<2;remote++){
   reset(v);int left=remote?BUTTON_RC_LEFT:BUTTON_LEFT;
   assert(lookup(left)==ACTION_STD_CANCEL&&lookup(left|BUTTON_REPEAT)==ACTION_AGENTS_LEFT_REPEAT);
   for(int phase=0;phase<4;phase++){
    int rawkey=left|(phase==3?BUTTON_REL:phase?BUTTON_REPEAT:0);
    int ret=dispatch(lookup(rawkey),rawkey);assert(!done&&!consents&&ret==AGENTS_EXIT_OTHER);cases++;
   }
   if(v==VIEW_USAGE||v==VIEW_DETAIL)assert(backspaces==3);
   if(v==VIEW_SPACES)assert(usage==1&&view==VIEW_USAGE&&backspaces==2);
   if(v==VIEW_AGENTS)assert(tabs==1&&!backspaces);
   if(v==VIEW_ALERT)assert(enters==1&&question_choice==q.yes);
   if(v==VIEW_QUESTION)assert(answers==1);
   if(v==VIEW_INPUT)assert(backspaces==1);
   struct result actual=result();if(remote)assert(!memcmp(&actual,&body,sizeof body));else body=actual;
   /* Repeats alone must not change a tab or activate a binary choice. */
   reset(v);dispatch(lookup(left|BUTTON_REPEAT),left|BUTTON_REPEAT);assert(!done&&!consents&&!tabs&&!usage&&!answers&&!enters);
   if(v==VIEW_USAGE||v==VIEW_DETAIL)assert(backspaces==1);else assert(!backspaces);
   /* Pending/busy and non-binary questions retain the body's gates. */
   for(int gate=0;gate<4;gate++){
    reset(v);if(gate==0)pending_req[0]='x';if(gate==1)focus_req[0]='x';if(gate==2)detail_ready=false;if(gate==3){q.yes=q.no=0;have_question=false;}
    dispatch(lookup(left),left);assert(!done&&!consents);cases++;
   }
   /* MENU alone retains the one-press exit, including busy questions. */
   reset(v);pending_req[0]=focus_req[0]='x';int menu=remote?BUTTON_RC_MENU:BUTTON_MENU;
   assert(dispatch(lookup(menu|BUTTON_REL),menu|BUTTON_REL)==AGENTS_EXIT_MENU&&done&&consents==1&&finishes==1);
   reset(v);assert(dispatch(ACTION_STD_CANCEL,menu|BUTTON_REL)==AGENTS_EXIT_MENU&&done&&consents==1);
  }
  views++;
  /* An untagged CANCEL is never a MENU consent, even when it exits. */
  reset(v);assert(dispatch(ACTION_STD_CANCEL,0)==AGENTS_EXIT_OTHER&&!consents);
  reset(v);quick_usb=true;assert(dispatch(ACTION_STD_QUICKSCREEN,0)==AGENTS_EXIT_OTHER&&done&&!consents);
  reset(v);assert(dispatch(SYS_USB_CONNECTED,0)==AGENTS_EXIT_OTHER&&!consents);
  assert(done==(v!=VIEW_ALERT&&v!=VIEW_INPUT));
  for(int a=0;a<4;a++){
   reset(v);int action[]={ACTION_AGENTS_COVERS,ACTION_AGENTS_REMOTE_COVERS,ACTION_AGENTS_SWITCH,ACTION_AGENTS_REMOTE_SWITCH};
   int want[]={AGENTS_SWITCH_COVERS,AGENTS_SWITCH_COVERS,AGENTS_SWITCH_WPS,AGENTS_SWITCH_REMOTE_WPS};
   int got=dispatch(action[a],0);
   if(v==VIEW_ALERT||v==VIEW_INPUT)assert(got==AGENTS_EXIT_OTHER&&!done&&!consents);
   else assert(got==want[a]&&done&&!consents);
  }
 }
 assert(failed_open()==AGENTS_EXIT_OTHER);
 for(int ret=AGENTS_EXIT_OTHER;ret<=AGENTS_EXIT_DEFERRED;ret++){consents=0;agents_usb_exit(ret);assert(!consents);}
 printf("FU45 Agents: %d views, %d LEFT/busy cases; body/remote press-repeat-release parity; MENU-only consent; failed open, CANCEL, USB, QuickScreen, switches/deferred safe PASS\n",views,cases);
}
'''
nav=run[run.index('        enum agents_nav_screen nav ='):run.index('        bool changed =')]
code+=r'''
#include "input/agents_shortcut.h"
static enum agents_nav_screen nav_pending;
enum agents_nav_screen agents_nav_pending(void){return nav_pending;}
bool agents_nav_take(enum agents_nav_screen n){assert(n==nav_pending);nav_pending=AGENTS_NAV_NONE;return true;}
static int pending_exit(enum agents_nav_screen n){int ret=AGENTS_EXIT_OTHER;nav_pending=n;do{
'''+nav+r'''
}while(false);agents_usb_exit(ret);return ret;}
'''
post=post.replace('assert(failed_open()==AGENTS_EXIT_OTHER);',
 'assert(failed_open()==AGENTS_EXIT_OTHER);consents=0;for(int v=VIEW_SPACES;v<=VIEW_INPUT;v++){view=v;assert(pending_exit(AGENTS_NAV_WPS)==AGENTS_SWITCH_WPS&&!consents);assert(pending_exit(AGENTS_NAV_COVERS)==AGENTS_SWITCH_COVERS&&!consents);}')
code+=post
# Only the explicit physical-MENU gate may assign the consent exit reason.
assert init.strip()=='int action, ret = AGENTS_EXIT_OTHER;'
assert run.count('ret = AGENTS_EXIT_MENU;')==1
menu_arm=run[run.index('        if (agents_menu_exit(action))'):run.index('        if (question_action')]
assert 'ret = AGENTS_EXIT_MENU;' in menu_arm
assert 'return 0;' not in run
(O/'config.h').write_text('');(O/'agents.c').write_text(code)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),str(O/'agents.c'),'-o',str(O/'agents')],check=True)
subprocess.run([str(O/'agents')],check=True,timeout=30)
