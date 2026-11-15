"""Production Apple remote decoder + native/Cider quickscreen action tables."""
from pathlib import Path
import re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
def fn(text,name):
 m=re.search(r'(?m)^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{',text);assert m,name
 e=m.end();d=1
 while d:d+=(text[e]=='{')-(text[e]=='}');e+=1
 return text[m.start():e]
key=(r/'apps-ipod/input/keymap_ipod.c').read_text();tables=[]
for name in ['remote_button_context_standard','remote_button_context_wps','remote_button_context_quickscreen','button_context_standard','button_context_wps','button_context_agents']:
 m=re.search(r'static const struct button_mapping '+name+r'\[\].*?\n};',key,re.S);assert m,name;tables.append(m.group())
volume=fn((r/'apps-ipod/input/action.c').read_text(),'remote_volume_action')
source=re.sub(r'^#include .*$', '',(r/'apps-ipod/input/agents_remote.c').read_text(),flags=re.M)
actions=sorted(set(re.findall(r'\bACTION_\w+','\n'.join(tables)+volume)))
pre=r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#define REMOTE_LOG_DISABLE
#include "firmware/export/remote_log.h"
#define IPOD_ACCESSORY_PROTOCOL
#include "firmware/target/arm/s5l8702/ipod6g/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#define BUTTON_MULTIMEDIA 0x10000000
#define SYS_EVENT 0x80000000u
#define HZ 100
#include "apps-ipod/input/agents_remote.h"
#define LAST_ITEM_IN_LIST {0,0,0}
#define LAST_ITEM_IN_LIST__NEXTLIST(c) {0,0,0}
struct button_mapping {int action_code,button_code,prereq_button_code;};
enum {CONTEXT_STD,CONTEXT_WPS,CONTEXT_QUICKSCREEN,CONTEXT_AGENTS,CONTEXT_LIST,CONTEXT_PLUGIN};
#define ACTIVITY_ALBUMCOVERS 2
static bool cider;
static bool cider_active(void){return cider;}
static int get_current_activity(void){return 0;}
typedef struct {int button,context,action;} action_cur_t;
'''
post=r'''
static int lookup(const struct button_mapping*m,int b,int prev){for(;m->button_code;m++)if(m->button_code==b&&(!m->prereq_button_code||m->prereq_button_code==prev))return m->action_code;return -1;}
static struct agents_remote_state state;
static int out[32],used;
static void feed(int context,int b,int held,unsigned t){int v=agents_remote_button(&state,context,b,held,t);if(v)out[used++]=v;while(state.queued){v=agents_remote_button(&state,context,0,held,t);if(v)out[used++]=v;}assert(used<32);}
int main(void){
 for(int source=0;source<2;source++){
  cider=source;
  for(int key=0;key<2;key++){
   int b=key?BUTTON_RC_VOL_DOWN:BUTTON_RC_VOL_UP;
   action_cur_t c={b|BUTTON_REL|BUTTON_REPEAT,CONTEXT_QUICKSCREEN,0};assert(remote_volume_action(&c)&&c.action==(key?ACTION_QS_DOWN:ACTION_QS_TOP));
   /* The same early-return branch serves the local and Cider quickscreens. */
   c.context=CONTEXT_STD;assert(remote_volume_action(&c)&&c.action==(key?ACTION_STD_NEXT:ACTION_STD_PREV));
   c.context=CONTEXT_WPS;assert(remote_volume_action(&c)==(bool)source);if(source)assert(c.action==(key?ACTION_WPS_VOLDOWN:ACTION_WPS_VOLUP));
  }
  const struct button_mapping *q=remote_button_context_quickscreen;
  assert(lookup(q,BUTTON_RC_LEFT,0)==ACTION_QS_LEFT);assert(lookup(q,BUTTON_RC_RIGHT,0)==ACTION_QS_RIGHT);
  assert(lookup(q,BUTTON_RC_LEFT|BUTTON_REPEAT,BUTTON_RC_LEFT)==ACTION_NONE);assert(lookup(q,BUTTON_RC_RIGHT|BUTTON_REPEAT,BUTTON_RC_RIGHT)==ACTION_NONE);
  assert(lookup(q,BUTTON_RC_MENU,0)==ACTION_NONE);assert(lookup(q,BUTTON_RC_MENU|BUTTON_REL,BUTTON_RC_MENU)==ACTION_STD_CANCEL);
  assert(lookup(q,BUTTON_RC_SELECT|BUTTON_REL,BUTTON_RC_SELECT)==ACTION_STD_CANCEL);assert(lookup(q,BUTTON_RC_PLAY|BUTTON_REL,BUTTON_RC_PLAY)==ACTION_STD_CANCEL);
  const struct button_mapping *w=remote_button_context_wps,*m=remote_button_context_standard;
  assert(lookup(w,BUTTON_RC_LEFT|BUTTON_REL,BUTTON_RC_LEFT)==ACTION_WPS_SKIPPREV);assert(lookup(w,BUTTON_RC_RIGHT|BUTTON_REL,BUTTON_RC_RIGHT)==ACTION_WPS_SKIPNEXT);
  assert(lookup(m,BUTTON_RC_LEFT,0)==ACTION_STD_CANCEL);assert(lookup(m,BUTTON_RC_RIGHT,0)==ACTION_STD_OK);
  assert(lookup(m,BUTTON_RC_SELECT|BUTTON_REL,BUTTON_RC_SELECT)==ACTION_STD_OK);assert(lookup(w,BUTTON_RC_SELECT|BUTTON_REL,BUTTON_RC_SELECT)==ACTION_WPS_BROWSE);
 }
 for(int context=REMOTE_OTHER;context<=REMOTE_ALBUM_LIST;context++){
  /* Centre (Dock Play) tap retains its context's behaviour. */
  state=(struct agents_remote_state){0};used=0;feed(context,BUTTON_RC_PLAY,BUTTON_RC_PLAY,100);feed(context,BUTTON_RC_PLAY|BUTTON_REL,0,110);feed(context,0,0,131);
  assert(out[used-2]==(context==REMOTE_AGENTS||context==REMOTE_COVERS||context==REMOTE_ALBUM_LIST?BUTTON_SELECT:BUTTON_RC_PLAY));
  /* Every non-Agents centre hold enters the native Play keymaps, no jump shortcut. */
  state=(struct agents_remote_state){0};used=0;feed(context,BUTTON_RC_PLAY,BUTTON_RC_PLAY,200);feed(context,0,BUTTON_RC_PLAY,200+(context==REMOTE_AGENTS?30:60));feed(context,BUTTON_RC_PLAY|BUTTON_REL,0,280);
  if(context!=REMOTE_AGENTS){assert(used==3&&out[0]==BUTTON_PLAY&&out[1]==(BUTTON_PLAY|BUTTON_REPEAT)&&out[2]==(BUTTON_PLAY|BUTTON_REL));}
  else {assert(used==2&&out[0]==BUTTON_RC_PLAY&&out[1]==(BUTTON_PLAY|BUTTON_REL));}
  /* Explicit EP0 Select: native Play hold, short Select preserved. */
  state=(struct agents_remote_state){0};used=0;feed(context,BUTTON_RC_SELECT,BUTTON_RC_SELECT,300);feed(context,BUTTON_RC_SELECT|BUTTON_REPEAT,BUTTON_RC_SELECT,330);feed(context,BUTTON_RC_SELECT|BUTTON_REL,0,350);
  assert(used==4&&out[0]==BUTTON_RC_SELECT&&out[1]==BUTTON_PLAY&&out[2]==(BUTTON_PLAY|BUTTON_REPEAT)&&out[3]==(BUTTON_PLAY|BUTTON_REL));
 }
 assert(lookup(button_context_wps,BUTTON_PLAY|BUTTON_REPEAT,BUTTON_PLAY)==ACTION_WPS_CONTEXT);
 assert(lookup(button_context_standard,BUTTON_PLAY|BUTTON_REPEAT,BUTTON_PLAY)==ACTION_NONE);
 assert(lookup(button_context_agents,BUTTON_PLAY,0)==ACTION_AGENTS_TALK);assert(lookup(button_context_agents,BUTTON_PLAY|BUTTON_REL,0)==ACTION_AGENTS_TALK_END);
 puts("FU30 every key in local/Cider quickscreen, native menu and WPS; centre tap preserved; native Play hold in all 5 scopes PASS");
}
'''
(o/'config.h').write_text('');(o/'test.c').write_text(pre+'enum {'+','.join(actions)+'};\n'+source+'\n'.join(tables)+volume+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(r),'-I'+str(o),str(o/'test.c'),'-o',str(o/'test')],check=True)
subprocess.run([str(o/'test')],check=True)
qs=(r/'apps-ipod/screens/playback/quick_screen.c').read_text()
assert 'menu_armed' not in qs, 'physical MENU retains TOP even when empty'
assert 'button_set_ui_timeout(raw_ui_timeout)' in (r/'apps-ipod/input/action.c').read_text()
