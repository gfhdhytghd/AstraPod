"""Production WPS Menu arm and root modal routing, without a UI/device."""
from pathlib import Path
import re
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve()
O.mkdir(parents=True, exist_ok=True)
root = (R/'apps-ipod/root_menu.c').read_text()
wps = (R/'apps-ipod/screens/playback/wps.c').read_text()
a = wps.index('            case ACTION_WPS_MENU:')
b = wps.index('            case ACTION_WPS_QUICKSCREEN:', a)
menu = wps[a:b]
source = r'''
#include <assert.h>
#include <stdbool.h>
static bool remote, covers, tracks;
static bool cider_active(void){return remote;}
static int cider_browse_covers(bool home){assert(!home);return 999;}
#include <stdio.h>
#define GO_TO_AGENTS 1
#define GO_TO_PREVIOUS 2
#define GO_TO_ALBUM_COVERS 3
#define GO_TO_ALBUM_COVERS_TRACKS 13
static bool cover_nav_playing(void) {return covers;}
static bool album_covers_reopen_tracks(void) {return tracks;}
#define ACTION_WPS_MENU 4
static void gwps_leave_wps(bool on){assert(on);}
static int menu(bool return_agents) {
 (void)return_agents;
 switch(ACTION_WPS_MENU) {
'''+menu+r'''
 }
 return -1;
}
int main(void) {
 for (int source=0;source<2;source++) {
  remote=source;
  assert(menu(true)==GO_TO_AGENTS);assert(menu(false)==GO_TO_PREVIOUS);
 }
 remote=false;covers=true;
 assert(menu(false)==GO_TO_ALBUM_COVERS);
 tracks=true;assert(menu(false)==GO_TO_ALBUM_COVERS_TRACKS);
 remote=true;assert(menu(false)==999);
 puts("WPS MENU: Agents and ordinary parents, local covers/tracks, and remote covers PASS");
}
'''
(O/'return.c').write_text(source)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-g',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                '-fno-pie','-no-pie',str(O/'return.c'),'-o',str(O/'return')],check=True)
subprocess.run([str(O/'return')],check=True)
# The flag is consumed before any no-audio/USB-audio early return.
wrapper = root[root.index('static int wpsscrn('):root.index('static int wpsscrn(')+1300]
assert wrapper.index('wps_return_agents = false') < wrapper.index('usb_audio_get_active()')
assert root.count('gui_wps_show(return_agents)') == 3
assert 'gui_wps_show(bool return_agents)' in wps
