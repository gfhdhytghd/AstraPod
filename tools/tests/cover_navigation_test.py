"""Any-screen native chord at the UI queue boundary; no hardware inputs."""
from pathlib import Path
import ast,re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
S=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else R
source=(S/'apps-ipod/input/action.c').read_text()
assert 'static long raw_ui_filter(long button)' in source, 'baseline has no any-screen raw UI hook'
def fn(t,n):
 m=re.search(r'(?m)^(?:static[ \t]+)?(?:long|void|int|bool)[ \t*]+'+n+r'\([^;{}]*\)\s*\{',t);assert m,n
 end=m.end();depth=1
 while depth:depth+=(t[end]=='{')-(t[end]=='}');end+=1
 return t[m.start():end]
a=ast.parse((R/'tools/tests/native_cover_chord_test.py').read_text())
pre=next(ast.literal_eval(n.value) for n in a.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
pre=pre.replace('#include "firmware/export/button_chord.h"',re.sub(r'^#include .*$', '',(R/'firmware/export/button_chord.h').read_text(),flags=re.M))
pre+='''
static void cider_source_poll(void){}
static void iap_volume_context(bool wps){(void)wps;}
static int iap_agents_volume_take(void){return 0;}
static int raw_volume_button(int b){return b;}
static bool ui_polling, plugin_owns_display;
static bool rock_ui_raw(void){return plugin_owns_display;}
static unsigned int ui_thread=7,caller=7;
static int held,clock_pumps;
static struct agents_cover_chord_state cover_chord;
static unsigned int thread_self(void){return caller;}
static int button_status(void){return held;}
static int cider_play_status(void){return 0;}
static void podlink_time_sync(void){clock_pumps++;}
'''
c=re.sub(r'^#include .*$', '',(S/'apps-ipod/input/agents_shortcut.c').read_text(),flags=re.M)
post=r'''
int main(void){
 int chord=BUTTON_SELECT|BUTTON_MENU;
 caller=8;held=chord;assert(raw_ui_filter(chord)==chord&&!clock_pumps&&!agents_nav_active());
 caller=7;plugin_owns_display=true;
 assert(raw_ui_filter(chord)==chord&&!clock_pumps&&!agents_nav_active());
 plugin_owns_display=false;caller=8;
#ifdef TEST_STAGGER_WHEEL
 assert(raw_ui_filter(BUTTON_SCROLL_FWD)==BUTTON_SCROLL_FWD&&!push_snaps);
 caller=7;ui_polling=true;
 assert(raw_ui_filter(BUTTON_SCROLL_BACK)==BUTTON_SCROLL_BACK&&push_snaps==1);
 assert(raw_ui_filter(SYS_EVENT|BUTTON_SCROLL_BACK)==(SYS_EVENT|BUTTON_SCROLL_BACK)&&push_snaps==1);
#endif
 caller=7;ui_polling=true;assert(raw_ui_filter(chord)==chord&&!clock_pumps);ui_polling=false;
 for(int screen=0;screen<9;screen++){
  /* Root menu/browser/settings/playlist/chart/dialog/game/WPS/Agents all
     share this thread-gated raw boundary, independent of activity. */
  held=chord;int out=raw_ui_filter(chord);
  assert(out==BUTTON_MENU&&agents_nav_pending()==AGENTS_NAV_COVERS);
  held=0;
  out=raw_ui_filter(BUTTON_SELECT|BUTTON_REL);assert(!(out&BUTTON_SELECT));
  out=raw_ui_filter(BUTTON_MENU|BUTTON_REL);assert(!(out&BUTTON_SELECT));
  assert(!cover_chord.latched);assert(agents_nav_take(AGENTS_NAV_COVERS));
 }
 held=chord;raw_ui_filter(chord);assert(agents_nav_pending()==AGENTS_NAV_COVERS);
 long a=raw_ui_filter(BUTTON_NONE),b=raw_ui_filter(BUTTON_NONE);
 assert(a!=b&&(a&BUTTON_MENU)&&(b&BUTTON_MENU));
 assert(raw_ui_filter(SYS_EVENT|1)==(SYS_EVENT|1));
 #ifdef TEST_LOCAL_WPS
 held=0;
 raw_ui_filter(chord|BUTTON_REL);
 assert(agents_nav_take(AGENTS_NAV_COVERS));
 const int wps=BUTTON_SELECT|BUTTON_PLAY;
 connected=true;held=wps;
 assert(raw_ui_filter(wps)==wps&&agents_nav_pending()==AGENTS_NAV_NONE);
 connected=false;
 assert(raw_ui_filter(wps)&BUTTON_MENU);
 assert(agents_nav_pending()==AGENTS_NAV_WPS);
 held=0;
 assert(raw_ui_filter(BUTTON_SELECT|BUTTON_REL)&BUTTON_MENU);
 assert(raw_ui_filter(BUTTON_PLAY|BUTTON_REL)&BUTTON_MENU);
 assert(!shortcut.latched&&agents_nav_take(AGENTS_NAV_WPS));
 assert(raw_ui_filter(BUTTON_SELECT)==BUTTON_SELECT);
 #endif
 puts("any-screen UI/raw SELECT+MENU: thread isolation, release suppression, repeat reuse, modal down/release cancellation PASS");
}
'''
(O/'config.h').write_text('')
pre+='\nstatic long raw_remote_filter(long button){return button;}\n'
wheel = ''
if 'static int wheel_shortcut_button(' in source:
 pre += '''
#define TEST_LOCAL_WPS
#define ACTION_NONE 0
#define ACTION_AGENTS_SWITCH 401
#define ACTION_TREE_WPS 402
static struct agents_shortcut_state shortcut;
static bool connected;
static bool podlink_host_connected(void){return connected;}
'''
 wheel = fn(source,'wheel_shortcut_button')
raw = fn(source,'raw_ui_filter')
if 'ui_scene_snap_push();' in raw:
 pre += '''
#define TEST_STAGGER_WHEEL
static int push_snaps;
static void ui_scene_snap_push(void){push_snaps++;}
'''
(O/'raw.c').write_text(pre+c+fn(source,'raw_ui_cancel')+wheel+raw+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(O),'-I'+str(R),'-I'+str(R/'apps-ipod'),str(O/'raw.c'),'-o',str(O/'raw')],check=True)
subprocess.run([str(O/'raw')],check=True)
root=(S/'apps-ipod/root_menu.c').read_text();q=(S/'firmware/drivers/button_queue.c').read_text()
assert 'agents_shortcut_init();' in root and 'if (ui_filter) return ui_filter(ev.id);' in q
route=fn(source,'route_screen_shortcut')
assert 'if (covers) { /* The owner/modal unwinds to the root dispatcher. */ }' in route
assert 'next_screen = GO_TO_ALBUM_COVERS;' in root
print('Raw button queue registration, deferred modal route, root dispatch wiring PASS')
