"""FU18 production WPS MENU arm and remote-entry lifetime; no UI/device."""
from pathlib import Path
import re,subprocess,sys
R=Path(__file__).resolve().parents[2];O=Path(sys.argv[1]).resolve();O.mkdir(parents=True,exist_ok=True)
def function(text,name):
 m=re.search(r'(?m)^(?:static[ \t]+)?(?:int|long|bool)[ \t*]+'+name+r'\([^;{}]*\)\s*\{',text);assert m,name
 end=m.end();depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[m.start():end]
root=(R/'apps-ipod/root_menu.c').read_text()
albums=(R/'apps-ipod/screens/covers/album_covers.c').read_text()
nav=r'''
#include <assert.h>
static void __attribute__((unused)) cider_remote_source(void){}
static void __attribute__((unused)) cider_source_poll(void){}
static int __attribute__((unused)) cider_wps_poll(void){return 1;}
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#define AGENTS_SWITCH_WPS 2
#define AGENTS_SWITCH_COVERS 3
#define AGENTS_SWITCH_REMOTE_WPS 4
#define GO_TO_PREVIOUS -1
#define GO_TO_WPS 10
#define GO_TO_CIDER 89
static bool __attribute__((unused)) cider_available(void){return false;}
static int __attribute__((unused)) cider_browse(void){return GO_TO_WPS;}
#define GO_TO_AGENTS 11
#define GO_TO_ROOT 12
#define GO_TO_ALBUM_COVERS_TRACKS 13
#define CAROUSEL_REMOTE_RETURN -22
#define CAROUSEL_CONTEXT -21
#define CAROUSEL_PLAY_ALBUM -20
#define AUDIO_STATUS_PLAY 1
static bool wps_return_agents,wps_select_remote,cover_toggled_back;
#define GO_TO_ALBUM_COVERS 14
static int agent_result,covers_result,playing=1;
static int agents_screen(void){return agent_result;}
static int album_covers_scrn(void*p){(void)p;return GO_TO_PREVIOUS;}
static int audio_status(void){return playing;}
static int false_leaves,true_leaves;
static void gwps_leave_wps(bool on){if(on)true_leaves++;else false_leaves++;}
static int album_model;
static bool tracks_album_valid;
static int carousel_run(int*m,const char*file){(void)m;(void)file;return covers_result;}
static int carousel_cover_menu(void){return GO_TO_ROOT;}
static void album_covers_cancel_resume(void){}
static int db_summary_play_pending(void){return 1;}
int album_covers_return(const char*,bool*);
'''
nav+='\n#define AGENTS_NAV_WPS 2\nstatic bool agents_nav_has(int screen){(void)screen;return false;}\nstatic bool cover_nav_active(void){return false;}\nstatic bool cover_nav_playing(void){return false;}\nstatic void cover_nav_begin(int i){(void)i;}\nstatic int cover_nav_finish(void){return GO_TO_PREVIOUS;}\nstatic void cover_nav_set_playing(bool b){(void)b;}\n'
nav+='\nstatic bool cider_active(void){return false;}\nstatic int cider_browse_covers(bool h){(void)h;return GO_TO_ROOT;}\n'
nav+=function(albums,'album_covers')+function(albums,'album_covers_return')
nav+=function((R/'apps-ipod/screens/playback/wps.c').read_text(),'show_album_covers')
nav+=function(root,'agents_scrn')
a=root.index('    bool return_agents =',root.index('static int wpsscrn('));b=root.index('    int audstatus',a)
nav+='\nstatic int consume(void){bool select_remote=wps_select_remote;'+root[a:b]+'return return_agents*2+select_remote;}\n'
nav+=r'''
int main(void){
 agent_result=AGENTS_SWITCH_REMOTE_WPS;assert(agents_scrn(NULL)==GO_TO_WPS);
 assert(wps_return_agents&&wps_select_remote);assert(consume()==3);assert(consume()==0);
 agent_result=AGENTS_SWITCH_WPS;assert(agents_scrn(NULL)==GO_TO_WPS);
 assert(wps_return_agents&&!wps_select_remote);assert(consume()==2);assert(consume()==0);
 agent_result=0;assert(agents_scrn(NULL)==GO_TO_PREVIOUS&&!wps_select_remote);
 bool remote=true;long go=0;
 covers_result=GO_TO_PREVIOUS;
 assert(show_album_covers(&go,false)&&go==GO_TO_PREVIOUS);
 assert(show_album_covers(&go,true)&&go==GO_TO_AGENTS);
 covers_result=CAROUSEL_REMOTE_RETURN;assert(!show_album_covers(&go,true));
 assert(album_covers(NULL)==GO_TO_PREVIOUS);
 covers_result=GO_TO_PREVIOUS;assert(album_covers_return(NULL,&remote)==GO_TO_PREVIOUS&&!remote);
 covers_result=GO_TO_ROOT;assert(show_album_covers(&go,false)&&go==GO_TO_ROOT);
 covers_result=GO_TO_ALBUM_COVERS_TRACKS;assert(show_album_covers(&go,false)&&go==GO_TO_ALBUM_COVERS_TRACKS);
 covers_result=CAROUSEL_REMOTE_RETURN;playing=0;
 assert(show_album_covers(&go,false)&&go==GO_TO_WPS);
 puts("Agents source selection is consumed once; local MENU ascends; the toggle resumes WPS; child exits propagate PASS");
}
'''
(O/'origins.c').write_text(nav)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(O/'origins.c'),'-o',str(O/'origins')],check=True)
subprocess.run([str(O/'origins')],check=True)
assert root.count('gui_wps_show(return_agents)')==3
assert root.index('wps_select_remote = false',root.index('static int wpsscrn('))<root.index('usb_audio_get_active()',root.index('static int wpsscrn('))
keys=(R/'apps-ipod/input/keymap_ipod.c').read_text()
for button in ['BUTTON_MENU','BUTTON_RC_MENU']:
 assert re.search(r'ACTION_WPS_MENU,\s*'+button+r'\|BUTTON_REL',keys)
assert re.search(r'ACTION_WPS_QUICKSCREEN,\s*BUTTON_MENU\|BUTTON_REPEAT',keys)
print('wheel/remote MENU share action; long MENU remains QuickScreen; provenance cleared before early WPS refusal PASS')
# Only inspect the actual GUI input switch, not similarly named party filters.
gui=(R/'apps-ipod/screens/playback/wps.c').read_text().split('long gui_wps_show(',1)[1]
a=gui.index('            case ACTION_WPS_STOP:');b=gui.index('            /* Bound only',a)
assert 'case ACTION_AGENTS_REMOTE_COVERS:' in gui[a:b]
assert 'case ACTION_AGENTS_COVERS:' in gui[a:b]
assert 'show_album_covers(&go_to, return_agents)' in gui[a:b]
assert 'case ACTION_AGENTS_REMOTE_SWITCH:' in gui
print('WPS cover/double actions belong to the real input switch (not just party filtering) PASS')
