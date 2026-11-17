"""Replay Dock button timing through the firmware recognizer and action dispatch."""
from pathlib import Path
import re
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
source = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else repo
out.mkdir(parents=True, exist_ok=True)
(out / 'config.h').write_text('')
inputs = source / 'apps-ipod/input'
remote = re.sub(r'^#include .*$', '', (inputs / 'agents_remote.c').read_text(), flags=re.M)
action = (inputs / 'action.c').read_text()
context = action[action.index('static int remote_context('):action.index('static int raw_ui_timeout(int timeout)\n{')]
dispatch = action[action.index('    if (*button == REMOTE_LYRICS_BUTTON)'):
                  action.index('\n#endif', action.index('    if (*button == REMOTE_LYRICS_BUTTON)'))]
viewer = (source / 'apps-ipod/viewers/lyric_viewer/lyric_viewer.c').read_text()
viewer_actions = viewer[viewer.index('            case ACTION_WPS_BROWSE: case ACTION_WPS_LYRICS:'):
                        viewer.index('            case ACTION_WPS_VOLUP: case ACTION_WPS_VOLDOWN:', viewer.index('            case ACTION_WPS_BROWSE: case ACTION_WPS_LYRICS:'))]
buttons = (inputs / 'keymap_ipod.c').read_text()
wps_keymap = buttons.split('remote_button_context_wps[]', 1)[1].split('LAST_ITEM_IN_LIST', 1)[0]
assert re.search(r'ACTION_WPS_PLAY,\s*BUTTON_RC_PLAY\|BUTTON_REL,\s*BUTTON_RC_PLAY', wps_keymap)
assert re.search(r'ACTION_WPS_LYRICS,\s*BUTTON_RC_PLAY\|BUTTON_REPEAT,\s*BUTTON_RC_PLAY', wps_keymap)
assert 'get_action(CONTEXT_WPS, timeout)' in viewer

header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REMOTE_LOG_DISABLE
#define HAVE_USB_PODLINK
#define IPOD_ACCESSORY_PROTOCOL
#include "firmware/target/arm/ipod/button-target.h"
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#define HZ 100
#include "firmware/export/remote_log.h"
#include "apps-ipod/input/agents_remote.h"

enum {
    CONTEXT_AGENTS, CONTEXT_WPS, CONTEXT_QUICKSCREEN, CONTEXT_PLUGIN,
    ACTIVITY_WPS, ACTIVITY_ALBUMCOVERS, ACTIVITY_DATABASEBROWSER, ACTIVITY_LYRICS,
    AGENTS_NAV_NONE, AGENTS_NAV_AGENTS, AGENTS_NAV_COVERS, AGENTS_NAV_TRACKS,
    ACTION_NONE, ACTION_WPS_BROWSE, ACTION_WPS_LYRICS, ACTION_WPS_MENU,
    ACTION_REMOTE_WPS, ACTION_AGENTS_REMOTE_SWITCH, ACTION_AGENTS_SWITCH,
    ACTION_AGENTS_REMOTE_COVERS, ACTION_AGENTS_COVERS
};
static int activity, pending;
static bool native;
static int get_current_activity(void) { return activity; }
static bool agents_nav_has(int target) { (void)target; return false; }
static void agents_nav_request(int target, bool remote)
{
    pending = target;
    native = !remote;
}
struct action_state { int button, action; bool wait_for_release; };
static struct action_state last;
'''
body = r'''
static bool gesture_action(struct action_state *last, int *button)
{
DISPATCH
    return false;
}

static bool lyric_action(int action)
{
    switch (action)
    {
VIEWER_ACTIONS
        default: return false;
    }
done:
    return true;
}

static struct agents_remote_state state;
static int mode, events[24], count;

static void sample(int button, int held, unsigned long tick)
{
    int context = remote_context(mode);
    int event = agents_remote_button(&state, context, button, held, tick);
    do {
        if (event) {
            assert(count < (int)(sizeof events / sizeof events[0]));
            gesture_action(&last, &event);
            events[count++] = event;
        }
        if (!state.queued) break;
        event = agents_remote_button(&state, context, BUTTON_NONE, held, tick);
    } while (true);
}

static void tap(unsigned long tick, unsigned int ticks)
{
    sample(BUTTON_RC_PLAY, BUTTON_RC_PLAY, tick);
    sample(BUTTON_RC_PLAY | BUTTON_REL, 0, tick + ticks);
}

static void fresh(void)
{
    state = (struct agents_remote_state){0};
    last = (struct action_state){BUTTON_SELECT, ACTION_WPS_BROWSE, true};
    pending = AGENTS_NAV_NONE;
    count = 0;
}

static void single(void)
{
    fresh();
    tap(100, 13);
    sample(0, 0, 133);
    assert(count == 0);
    sample(0, 0, 134);
    int button = mode == CONTEXT_WPS ? BUTTON_RC_PLAY : BUTTON_SELECT;
    assert(count == 2 && events[0] == button && events[1] == (button | BUTTON_REL));
}

static void multi_tap(void)
{
    fresh();
    tap(100, 29);
    tap(144, 13);
    sample(0, 0, 177);
    assert(count == 0);
    sample(0, 0, 178);
    assert(count == 1 && events[0] == ACTION_AGENTS_REMOTE_SWITCH);

    fresh();
    tap(100, 29);
    tap(144, 13);
    tap(172, 13);
    sample(0, 0, 250);
    assert(count == 1 && events[0] == ACTION_AGENTS_REMOTE_COVERS);
}

static void held_then_taps(bool triple)
{
    fresh();
    sample(BUTTON_RC_PLAY, BUTTON_RC_PLAY, 300);
    sample(BUTTON_RC_PLAY | BUTTON_REPEAT, BUTTON_RC_PLAY, 359);
    assert(count == 0);
    sample(0, BUTTON_RC_PLAY, 360);
    if (mode == CONTEXT_WPS) {
        assert(count == 1 && events[0] == ACTION_WPS_LYRICS);
    } else {
        assert(count == 2 && events[0] == BUTTON_PLAY &&
               events[1] == (BUTTON_PLAY | BUTTON_REPEAT));
    }
    /* A redraw can observe the sampled up edge before a previously queued
     * repeat and the explicit release reach the action consumer. */
    sample(0, 0, 370);
    int after_release = count;
    sample(BUTTON_RC_PLAY | BUTTON_REPEAT, 0, 371);
    sample(BUTTON_RC_PLAY | BUTTON_REL, 0, 372);
    tap(380, 13);
    tap(408, 13);
    if (triple) tap(436, 13);
    sample(0, 0, 480);
    assert(count == after_release + 1);
    assert(events[after_release] == (triple ? ACTION_AGENTS_REMOTE_COVERS : ACTION_AGENTS_REMOTE_SWITCH));
    if (mode == CONTEXT_WPS)
        assert(last.button == BUTTON_REL && !last.wait_for_release);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    activity = !strcmp(argv[1], "WPS") ? ACTIVITY_WPS :
               !strcmp(argv[1], "Cover Flow") ? ACTIVITY_ALBUMCOVERS : ACTIVITY_LYRICS;
    mode = activity == ACTIVITY_ALBUMCOVERS ? CONTEXT_PLUGIN : CONTEXT_WPS;
    switch (atoi(argv[2])) {
        case 0: single(); multi_tap(); break;
        case 1: held_then_taps(false); held_then_taps(true); break;
        case 2:
            assert(activity == ACTIVITY_LYRICS);
            fresh();
            assert(lyric_action(ACTION_AGENTS_REMOTE_SWITCH) && pending == AGENTS_NAV_AGENTS && !native);
            assert(lyric_action(ACTION_AGENTS_REMOTE_COVERS) && pending == AGENTS_NAV_COVERS && !native);
            assert(lyric_action(ACTION_WPS_LYRICS));
            break;
        default: assert(0);
    }
    return 0;
}
'''
(out / 'remote-play.c').write_text(header + remote + context +
                                    body.replace('DISPATCH', dispatch).replace('VIEWER_ACTIONS', viewer_actions))
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I'+str(out), '-I'+str(source), str(out/'remote-play.c'),
                '-o', str(out/'remote-play')], check=True)
failures = []
for name in ('WPS', 'Cover Flow', 'Lyrics'):
    for case in range(3 if name == 'Lyrics' else 2):
        result = subprocess.run([str(out/'remote-play'), name, str(case)], timeout=5)
        print(name, ('single/double/triple', 'hold then double/triple', 'lyrics dispatch')[case],
              'PASS' if result.returncode == 0 else 'FAIL', flush=True)
        if result.returncode:
            failures.append((name, case))
assert not failures, failures
