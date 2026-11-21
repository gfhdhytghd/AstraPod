"""Exercise firmware root and cover controllers with scripted MENU and toggle actions."""
from pathlib import Path
import re
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source_repo = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else repo
adapter = source_repo / 'tools/tests/fu43_covers_nav_test.py'
namespace = {'__file__': str(adapter), '__name__': 'fu66_adapters'}
saved_argv = sys.argv
try:
    sys.argv = [str(adapter), str(out)]
    exec(compile(adapter.read_text().split("subprocess.run(['cc'")[0],
                 str(adapter), 'exec'), namespace)
finally:
    sys.argv = saved_argv

source = namespace['source']
function = namespace['fn']
wps = (source_repo / 'apps-ipod/screens/playback/wps.c').read_text()
old_wps = 'bool menu_covers' in function(wps, 'gui_wps_show')
menu = wps[wps.index('            case ACTION_WPS_MENU:'):
           wps.index('            case ACTION_WPS_QUICKSCREEN:')]
toggle = wps[wps.index('            case ACTION_WPS_STOP:', wps.index('static long wps_run')):
             wps.index('            /* Bound only by a keymap')]
source = source.replace('enum current_activity {', 'enum current_activity {ACTION_WPS_MENU=900,ACTION_WPS_STOP,')
source = source.replace('static bool album_covers_reopen_tracks(void){return true;}',
                        'static bool album_covers_reopen_tracks(void){return false;}')
source = source.replace('static int action_budget;', 'static int action_budget;\nstatic int take_step(int);')
source = source.replace(function(source, 'list_do_action'), r'''
static bool list_do_action(int context,int timeout,struct gui_synclist*l,int*a)
{
    (void)context; (void)timeout;
    assert(depth == 0);
    *a = take_step(GO_TO_CIDER);
    l->selected = *a == ACTION_STD_OK ? 0 : l->selected;
    return false;
}
''')
source = source.replace(function(source, 'agents_screen'), r'''
static int agents_screen(void)
{
    int action = take_step(GO_TO_AGENTS);
    if (action == ACTION_AGENTS_REMOTE_SWITCH) return AGENTS_SWITCH_REMOTE_WPS;
    if (action == ACTION_AGENTS_SWITCH) return AGENTS_SWITCH_WPS;
    assert(action == ACTION_STD_MENU);
    return 0;
}
''')
source = source.replace(function(source, 'cider_covers'), r'''
int cider_covers(void)
{
    assert(active && available && !claimed);
    assert(agents_nav_enter(AGENTS_NAV_COVERS));
    claimed = true;
    int result = cover_event(take_step(GO_TO_ALBUM_COVERS));
    claimed = false;
    agents_nav_leave(AGENTS_NAV_COVERS);
    return result == CAROUSEL_REMOTE_RETURN ? CIDER_COVERS_REMOTE_RETURN : result;
}
''')
source = source.replace(function(source, 'album_covers_return'), r'''
static int album_covers_return(const char *file, bool *toggled)
{
    (void)file;
    assert(!claimed && agents_nav_enter(AGENTS_NAV_COVERS));
    claimed = true;
    int result = cover_event(take_step(GO_TO_ALBUM_COVERS));
    claimed = false;
    agents_nav_leave(AGENTS_NAV_COVERS);
    *toggled = result == CAROUSEL_REMOTE_RETURN;
    return *toggled ? GO_TO_PREVIOUS : result;
}
''')
source = source.replace(function(source, 'gui_wps_show'), r'''
COVER_SOURCE
static long gui_wps_show(bool return_agentsOLD_PARAMETER)
{
    bool restore = false, theme_enabled = true;
    assert(agents_nav_enter(AGENTS_NAV_WPS));
    for (;;) {
        int button = take_step(GO_TO_WPS);
        switch (button) {
MENU
TOGGLE
        default: assert(0);
        }
    }
}

static long instrumented_wps(bool return_agentsOLD_PARAMETER)
{
    long result = gui_wps_show(return_agentsOLD_ARGUMENT);
    agents_nav_leave(AGENTS_NAV_WPS);
    return result;
}
'''.replace('COVER_SOURCE', '' if old_wps else function(wps, 'select_cover_source'))
   .replace('OLD_PARAMETER', ', bool menu_covers' if old_wps else '')
   .replace('OLD_ARGUMENT', ', menu_covers' if old_wps else '')
   .replace('MENU\n', menu).replace('TOGGLE', toggle))
source = source.replace('ret_val = gui_wps_show(', 'ret_val = instrumented_wps(')
source = source.replace(function(source, 'generic'), r'''
static int generic(void *parameter)
{
    int screen = (int)(intptr_t)parameter;
    int action = take_step(screen);
    if (action == ACTION_AGENTS_COVERS) return GO_TO_ALBUM_COVERS;
    if (action == ACTION_REMOTE_WPS) return GO_TO_WPS;
    assert(action == ACTION_STD_MENU);
    return GO_TO_ROOT;
}
''')
source = source.replace(function(source, 'main'), r'''
struct step { int screen, action; };
static const struct step *steps;
static size_t step_count, step_position;
static int take_step(int screen)
{
    if (step_position >= step_count || steps[step_position].screen != screen) {
        fprintf(stderr, "step %zu wanted screen %d, entered %d\n",
                step_position, step_position < step_count ? steps[step_position].screen : GO_TO_ROOT,
                screen);
        exit(1);
    }
    return steps[step_position++].action;
}

#define WPS_MENU {GO_TO_WPS, ACTION_WPS_MENU}
#define COVER_MENU {GO_TO_ALBUM_COVERS, ACTION_STD_MENU}
#define OPEN_WPS {GO_TO_ALBUM_COVERS, ACTION_REMOTE_WPS}
#define TOGGLE_COVER {GO_TO_WPS, ACTION_AGENTS_REMOTE_COVERS}
#define BACK_TOGGLE {GO_TO_ALBUM_COVERS, ACTION_AGENTS_REMOTE_COVERS}
#define AGENTS_MENU {GO_TO_AGENTS, ACTION_STD_MENU}
#define MUSIC_MENU {music, ACTION_STD_MENU}
#define PLAY_FROM_MUSIC {music, ACTION_REMOTE_WPS}
#define CHECK(entry, ...) do { \
    const struct step sequence[] = {__VA_ARGS__}; \
    reset(source); \
    script = 1; \
    step_position = 0; \
    step_count = ARRAYLEN(sequence); \
    steps = sequence; \
    run(entry); \
    assert(step_position == step_count); \
    assert(!claimed && !agents_nav_active() && !cover_nav_active() && activity_count == 0); \
} while (0)

int main(int argc, char **argv)
{
    assert(argc == 3);
    int source = atoi(argv[1]), scenario = atoi(argv[2]);
    for (int screen = 0; screen <= GO_TO_CIDER; screen++)
        items[screen] = (struct root_item){generic, (void *)(intptr_t)screen};
    items[GO_TO_CIDER] = (struct root_item){cider_scrn, NULL};
    items[GO_TO_ALBUM_COVERS] = (struct root_item){album_covers_scrn, NULL};
    items[GO_TO_WPS] = (struct root_item){wpsscrn, NULL};
    items[GO_TO_AGENTS] = (struct root_item){agents_scrn, NULL};
    int music = source ? GO_TO_CIDER : GO_TO_DBBROWSER;

    switch (scenario) {
    case 0: CHECK(GO_TO_ALBUM_COVERS, OPEN_WPS, WPS_MENU, COVER_MENU); break;
    case 1: CHECK(GO_TO_WPS, TOGGLE_COVER, COVER_MENU); break;
    case 2: CHECK(GO_TO_WPS, TOGGLE_COVER, BACK_TOGGLE, WPS_MENU); break;
    case 3: CHECK(GO_TO_AGENTS, {GO_TO_AGENTS, ACTION_AGENTS_REMOTE_SWITCH}, WPS_MENU, AGENTS_MENU); break;
    case 4: CHECK(music, {music, source ? ACTION_STD_OK : ACTION_AGENTS_COVERS},
                  OPEN_WPS, WPS_MENU, COVER_MENU, MUSIC_MENU); break;
    case 5: CHECK(music, PLAY_FROM_MUSIC, TOGGLE_COVER, COVER_MENU, MUSIC_MENU); break;
    case 6: CHECK(GO_TO_AGENTS, {GO_TO_AGENTS, ACTION_AGENTS_SWITCH},
                  TOGGLE_COVER, COVER_MENU, AGENTS_MENU); break;
    case 7: CHECK(music, PLAY_FROM_MUSIC, TOGGLE_COVER, BACK_TOGGLE, WPS_MENU, MUSIC_MENU); break;
    default: assert(0);
    }
    printf("source %d, route %d: Home reached after %zu scripted actions\n", source, scenario, step_count);
    return 0;
}
''')
source = source.replace('#include <assert.h>', '#include <assert.h>\n#include <stdlib.h>', 1)
(out / 'navigation.c').write_text(source)
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-enum-conversion', '-Wno-unused-function', '-Wno-unused-variable',
                '-Wno-unused-but-set-variable', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-I'+str(out), '-I'+str(source_repo),
                '-I'+str(source_repo/'apps-ipod'), str(out/'navigation.c'),
                '-o', str(out/'navigation')], check=True)
failures = []
for backend in range(3):
    for scenario in range(8):
        result = subprocess.run([str(out/'navigation'), str(backend), str(scenario)], timeout=5)
        if result.returncode:
            failures.append((backend, scenario))
assert not failures, failures
