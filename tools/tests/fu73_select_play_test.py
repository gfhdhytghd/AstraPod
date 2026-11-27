"""Exercise the click-wheel sampler and action poller without device access."""
from pathlib import Path
import re
import subprocess
import sys

repo = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
source = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else repo
out.mkdir(parents=True, exist_ok=True)
(out / 'config.h').write_text('')


def read(path):
    return (source / path).read_text()


def without_includes(path):
    return re.sub(r'^#include .*$', '', read(path), flags=re.M)


def function(body, name):
    start = re.search(r'(?m)^[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', body)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (body[end] == '{') - (body[end] == '}')
        end += 1
    return body[start.start():end]


action = read('apps-ipod/input/action.c')
driver = read('firmware/drivers/button.c')
keymap = read('apps-ipod/input/keymap_ipod.c')
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define HAVE_USB_PODLINK
#define HZ 100
#define BUTTON_NONE 0
#define BUTTON_REL 0x02000000
#define BUTTON_REPEAT 0x04000000
#define BUTTON_REDRAW 0x20000000
#define BUTTON_MULTIMEDIA 0x10000000
#define SYS_EVENT 0x80000000u
#include "firmware/target/arm/ipod/button-target.h"
#define BUTTON_REMOTE 0
#include "apps-ipod/input/agents_shortcut.h"
'''
fixtures = r'''
enum { ACTIVITY_HOME, ACTIVITY_BROWSER, ACTIVITY_SETTINGS, ACTIVITY_ALBUMCOVERS,
       ACTIVITY_LYRICS, ACTIVITY_WPS, ACTIVITY_AGENTS };
enum { PF_SORTING_NEXT = 2000, PF_SORTING_PREV, PF_RANDOM, PF_QUIT };
static int activity, sampled, lastbtn, lastdata;
static int queue[4], nqueued;
static unsigned long current_tick, shortcut_talking_at;
static bool connected, shortcut_talking, ui_polling;
static struct agents_shortcut_state shortcut;
static struct agents_cover_chord_state cover_chord;

static bool podlink_host_connected(void) { return connected; }
static int get_current_activity(void) { return activity; }
static bool button_queue_try_post(long id, int data)
{
    (void)data;
    if (nqueued && !(id & BUTTON_REL)) return false;
    assert(nqueued < (int)(sizeof(queue) / sizeof(*queue)));
    queue[nqueued++] = id;
    return true;
}
static int button_get_w_tmo(int timeout)
{
    (void)timeout;
    if (!nqueued) return BUTTON_NONE;
    int button = queue[0];
    --nqueued;
    for (int i = 0; i < nqueued; ++i) queue[i] = queue[i + 1];
    return button;
}
static int button_read(int *data) { *data = 0; return sampled; }
static int button_status(void) { return lastbtn; }
static void button_remote_post(void) {}
static void check_audio_peripheral_state(void) {}
static void reset_poweroff_timer(void) {}
static void podlink_time_sync(void) {}
static void agents_background_play_tone(void) {}
static bool has_flag(int value, int flag) { return (value & flag) == flag; }
typedef struct { int button, action, context; bool wait_for_release; } action_last_t;
typedef struct { int button, context, timeout; } action_cur_t;
'''

wheel = (function(action, 'wheel_shortcut_button')
         if 'static int wheel_shortcut_button(' in action else '')
repeat_macros = '\n'.join(re.findall(r'^#define REPEAT_.*$', driver, flags=re.M))
tables = []
for name in ('button_context_standard', 'button_context_tree', 'button_context_wps',
             'button_context_settings_grid', 'button_context_yesno', 'button_context_agents'):
    match = re.search(r'static const struct button_mapping ' + name + r'\[\].*?\n};',
                      keymap, flags=re.S)
    assert match, name
    tables.append(match[0])
covers = read('apps-ipod/screens/covers/carousel.c')
start = covers.index('const struct button_mapping pf_context_buttons[]')
tables.append(covers[start:covers.index('\n};', start) + 3])
viewer = read('apps-ipod/viewers/lyric_viewer/lyric_viewer.c')
lyrics_cases = ''
if 'case ACTION_UNKNOWN:' in viewer:
    entry = ('static int lyric_viewer_run(' if 'static int lyric_viewer_run(' in viewer
             else 'int lyric_viewer(void)')
    start = viewer.index('switch (action)', viewer.index(entry))
    lyrics_cases = viewer[start:viewer.index('case ACTION_WPS_VOLUP:', start)]
    lyrics_cases += 'default: break;\n}'
lyrics_commit = (function(action, 'agents_shortcut_commit_select')
                 if 'void agents_shortcut_commit_select(' in action else '')
test = r'''
static action_last_t last;
static int unwanted, wps_requests, agents_actions;
static const struct button_mapping *map;
static bool lyrics_probe, seek_ready;
static int seeks, lyrics_exits, browses, play_actions;
static struct { bool seek_consumed; } lv;

int get_action_statuscode(int *button) { *button = last.button; return 0; }
static bool lv_select_seek(uint32_t pressed)
{
    (void)pressed;
    if (!seek_ready) return false;
    seek_ready = false;
    ++seeks;
    return true;
}
static void lyrics_action(int action)
{
    uint32_t pressed = current_tick;
''' + (lyrics_cases or '(void)action; (void)pressed; goto done;') + r'''
    return;
done:
    ++lyrics_exits;
    activity = ACTIVITY_WPS;
}

static int map_button(int button)
{
    for (const struct button_mapping *key = map; key->button_code; ++key)
        if (key->button_code == button &&
            (!key->pre_button_code || key->pre_button_code == last.button))
            return key->action_code;
    return lyrics_probe ? ACTION_UNKNOWN : ACTION_NONE;
}

static void poll_action(void)
{
    action_cur_t current = {.context = last.context};
    int action;
    if (action_poll_button(&last, &current))
        action = current.button;
    else
    {
        action = map_button(current.button);
        last.button = current.button;
    }
    if (lyrics_probe && activity == ACTIVITY_LYRICS)
        lyrics_action(action);
    if (action == ACTION_WPS_BROWSE) ++browses;
    if (action == ACTION_WPS_PLAY || action == ACTION_WPS_HOTKEY) ++play_actions;
    switch (action)
    {
        case ACTION_AGENTS_SWITCH: ++agents_actions; break;
        case ACTION_NONE: case ACTION_WPS_MENU: break;
        default: ++unwanted; break;
    }
    if (agents_nav_take(AGENTS_NAV_WPS))
    {
        ++wps_requests;
        /* The key-up can arrive after the WPS or the Agents screen opens. */
        map = button_context_wps;
        last.context = CONTEXT_WPS;
    }
}

static void sample(int buttons, int ticks, bool drain)
{
    sampled = buttons;
    for (int i = 0; i < ticks; ++i)
    {
        ++current_tick;
        button_tick();
        if (drain) do { poll_action(); } while (nqueued);
    }
}

static int check_lyrics(int order)
{
    const int pair = BUTTON_SELECT | BUTTON_PLAY;
    lyrics_probe = seek_ready = true;
    if (order == 4 || order == 5)
    {
        sample(BUTTON_SELECT, order == 4 ? 7 : 2, true);
        if (order == 4) assert(seeks == 1 && lyrics_exits == 0);
        sample(0, 1, true);
        assert(seeks == 1 && lyrics_exits == 0 && !lv.seek_consumed);
        assert(browses == 1 && play_actions == 0);
        printf("Lyrics | single SELECT hold=%d | one seek\n", order == 4);
        return 0;
    }

    if (order)
        sample(order == 2 ? BUTTON_PLAY : BUTTON_SELECT, order == 3 ? 7 : 6, true);
    sample(pair, 100, true);
    sample(BUTTON_SELECT, 1, false);
    sample(0, 1, false);
    while (nqueued) poll_action();

    printf("Lyrics | connected=%d order=%d | seek=%d exit=%d WPS=%d Agents=%d\n",
           connected, order, seeks, lyrics_exits, wps_requests, agents_actions);
    assert(browses == 0 && play_actions == 0);
    if (order != 3)
        return seeks || lyrics_exits != 1 || wps_requests != !connected ||
               agents_actions != connected;

    if (seeks != 1 || lyrics_exits || wps_requests || agents_actions)
        return 1;
    /* Once both keys are up, a new chord can exit without a second seek. */
    seek_ready = true;
    sample(pair, 2, true);
    sample(0, 1, true);
    return seeks != 1 || lyrics_exits != 1 || wps_requests != !connected ||
           agents_actions != connected;
}

int main(int argc, char **argv)
{
    const int pair = BUTTON_SELECT | BUTTON_PLAY;
    const struct {
        const char *name;
        int context;
        const struct button_mapping *map;
    } screens[] = {
        {"Home", CONTEXT_MAINMENU, button_context_tree},
        {"Files", CONTEXT_TREE, button_context_tree},
        {"Settings grid", CONTEXT_SETTINGS_TIME, button_context_settings_grid},
        {"Cover Flow", CONTEXT_PLUGIN, pf_context_buttons},
        {"Lyrics", CONTEXT_WPS, button_context_wps},
        {"WPS", CONTEXT_WPS, button_context_wps},
        {"Agents", CONTEXT_AGENTS, button_context_agents},
        {"Yes/No", CONTEXT_YESNOSCREEN, button_context_yesno},
    };
    assert(argc == 4 || argc == 5);
    connected = !!atoi(argv[1]);
    activity = atoi(argv[2]);
    int order = atoi(argv[3]);
    map = screens[activity].map;
    last = (action_last_t){.button = BUTTON_REL, .context = screens[activity].context};
    sample(0, 4, true);
    if (argc == 5) return check_lyrics(order);
    if (order)
        sample(order == 1 ? BUTTON_SELECT : BUTTON_PLAY, 3, true);
    sample(pair, 100, true);
    /* Both releases arrive in the queue before the next action poll. */
    sample(order == 2 ? BUTTON_PLAY : BUTTON_SELECT, 1, false);
    sample(0, 1, false);
    while (nqueued) poll_action();

    printf("%s | connected=%d order=%d | WPS=%d Agents=%d leaks=%d\n",
           screens[activity].name, connected, order, wps_requests, agents_actions, unwanted);
    if (unwanted || wps_requests != !connected || agents_actions != connected)
        return 1;

    /* A subsequent rapid standalone tap still has its down and release. */
    last.context = CONTEXT_WPS;
    map = button_context_wps;
    sample(BUTTON_SELECT, 2, true);
    sample(0, 1, true);
    sample(BUTTON_PLAY, 2, true);
    sample(0, 1, true);
    assert(unwanted == 2);
    return 0;
}
'''
probe = out / 'test.c'
probe.write_text(prelude + without_includes('firmware/export/button_chord.h') +
                 without_includes('apps-ipod/input/action.h') +
                 without_includes('apps-ipod/input/agents_shortcut.c') +
                 fixtures + repeat_macros + '\n' + function(driver, 'button_tick') +
                 '\n' + wheel + '\n' + lyrics_commit + '\n' + function(action, 'action_poll_button') +
                 '\n' + '\n'.join(tables) + test)
exe = out / 'test'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-variable', '-Wno-unused-function', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                f'-I{out}', f'-I{source}', f'-I{source / "apps-ipod"}',
                str(probe), '-o', str(exe)], check=True)
failures = 0
for connected in range(2):
    for screen in range(8):
        for order in range(3):
            failures += subprocess.run([str(exe), str(connected), str(screen), str(order)]).returncode != 0
print(f'{48 - failures}/48 passed', flush=True)
if lyrics_cases:
    lyric_failures = 0
    for connected in range(2):
        for order in range(6):
            lyric_failures += subprocess.run(
                [str(exe), str(connected), '4', str(order), 'lyrics']).returncode != 0
    print(f'{12 - lyric_failures}/12 lyrics chord/seek cases passed', flush=True)
    failures += lyric_failures
raise SystemExit(bool(failures))
