"""Check body/remote parity for Agents direction actions and MENU consent."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
SOURCE = (ROOT / 'apps-ipod/screens/system/agents_screen.c').read_text()
KEYMAP = (ROOT / 'apps-ipod/input/keymap_ipod.c').read_text()


def function(name):
    match = re.search(r'(?m)^static (?:bool|void) ' + name + r'\([^;{}]*\)\s*\{', SOURCE)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


# Keep release-only taps routable. The screen tracks completed holds and
# requires a post-grace down edge before routing an approval choice.
for table, prefix in (('button_context_agents', 'BUTTON_'),
                      ('remote_button_context_agents', 'BUTTON_RC_')):
    entries = re.search(r'static const struct button_mapping ' + table +
                        r'\[\].*?\n};', KEYMAP, re.S).group()
    for key in ('LEFT', 'RIGHT'):
        button = prefix + key
        for action, suffix, preceding in (
            ('ACTION_AGENTS_' + key + '_DOWN', '', 'BUTTON_NONE'),
            ('ACTION_AGENTS_' + key, '|BUTTON_REL', 'BUTTON_NONE'),
            ('ACTION_AGENTS_' + key + '_REPEAT', '|BUTTON_REPEAT', 'BUTTON_NONE'),
        ):
            pattern = (r'\{\s*' + action + r',\s*' + re.escape(button + suffix) +
                       r',\s*' + preceding + r'\s*\}')
            assert re.search(pattern, entries), (table, key, action)

views = re.search(r'enum view \{[^}]+\};', SOURCE).group()
assert SOURCE.count('ret = AGENTS_EXIT_MENU;') == 1
assert re.search(r'if \(agents_menu_exit\(action\)\).*?ret = AGENTS_EXIT_MENU;',
                 SOURCE, re.S)

prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_USB_CONFIRM
#include "screens/system/agents_screen.h"
#include "screens/system/herdremote_model.h"
#include "firmware/target/arm/ipod/button-target.h"
#define ACTION_STD_CANCEL 3
#define ACTION_STD_MENU 4
VIEWS
struct gui_synclist {int dummy;};
static enum view view;
static bool fruit, t3, flat, confirm_selected, alert_direction_ready, detail_ready, question_closing;
static int calls, raw, direction, detail_scroll, detail_scroll_max = 16, consent, tab;
static char detail_pane[16] = "t3t1";
static struct hr_question question = {.yes=1, .no=2};
enum { NONE, TAB, USAGE, PROJECTS, BACKSPACE, FILTER, PAGE, ACTIONS, ALERT, DRAW };
static bool agents_fruit_nav(void) {return fruit;}
static bool usage_legacy_keys(void) {return !fruit && !t3;}
static bool is_t3(const char *s) {return !strncmp(s,"t3",2);}
static bool t3_flat(void) {return flat;}
static int tab_index(void) {return tab;}
static void select_tab(struct gui_synclist *l,int index) {(void)l; calls=TAB; tab=index;}
static void open_usage(struct gui_synclist *l) {(void)l; calls=USAGE;}
static void close_usage(struct gui_synclist *l) {(void)l; calls=PROJECTS;}
static void send_key(const char *key) {assert(!strcmp(key,"bs")); calls=BACKSPACE;}
static void switch_tab(struct gui_synclist *l, int dir) {(void)l; calls=FILTER; direction=dir;}
static void scroll_panel(struct gui_synclist *l,int dir,bool page) {(void)l; assert(page); calls=PAGE; direction=dir;}
static void draw_detail(struct gui_synclist *l) {(void)l; calls=DRAW;}
static void request_actions(void) {calls=ACTIONS;}
static const struct hr_question *current_question(void) {return &question;}
static int quick_choice(const struct hr_question *q,bool deny) {return deny ? q->no : q->yes;}
static void alert_activate(struct gui_synclist *l,int choice) {(void)l; calls=ALERT; direction=choice ? 1 : -1;}
static void draw_question_overlay(struct gui_synclist *l,bool animate) {(void)l; assert(!animate); calls=DRAW;}
static int get_action_statuscode(int *b) {*b=raw; return 0;}
static void usb_confirm_reopen(void) {consent++;}
'''.replace('VIEWS', views)

checks = r'''
static void check(int screen, int dir, bool held, int expected)
{
    view=screen; calls=NONE; alert_direction_ready=true;
    struct gui_synclist list={0};
    horizontal_button(&list,dir,held);
    assert(calls==expected);
    if (expected==FILTER ||
        expected==PAGE || expected==ALERT)
        assert(direction==dir);
}
int main(void)
{
    t3=true;
    check(VIEW_SPACES,-1,false,USAGE);
    check(VIEW_SPACES,1,false,NONE);
    flat=true;
    check(VIEW_SPACES,1,false,TAB); assert(tab==1);
    check(VIEW_SPACES,1,true,NONE); assert(tab==1);
    check(VIEW_SPACES,1,false,TAB); assert(tab==0);
    fruit=true;
    check(VIEW_SPACES,1,false,NONE);
    fruit=flat=false;
    check(VIEW_USAGE,-1,true,NONE);
    check(VIEW_USAGE,1,false,PROJECTS);
    check(VIEW_AGENTS,-1,false,FILTER);
    check(VIEW_AGENTS,1,true,NONE);
    check(VIEW_DETAIL,-1,false,PAGE);
    check(VIEW_DETAIL,1,true,DRAW); assert(detail_scroll==detail_scroll_max);
    check(VIEW_DETAIL,-1,true,DRAW); assert(detail_scroll==0);
    check(VIEW_ALERT,-1,false,ALERT);
    check(VIEW_ALERT,1,false,ALERT);
    check(VIEW_ALERT,-1,true,NONE);
    check(VIEW_QUESTION,-1,false,NONE);
    check(VIEW_ACTIONS,1,false,NONE);
    check(VIEW_CONFIRM,1,false,DRAW); assert(confirm_selected);
    check(VIEW_CONFIRM,-1,false,DRAW); assert(!confirm_selected);
    strcpy(detail_pane,"p1"); t3=false; detail_ready=true;
    check(VIEW_USAGE,-1,true,BACKSPACE);
    check(VIEW_DETAIL,-1,false,BACKSPACE);
    check(VIEW_DETAIL,1,false,ACTIONS);
    check(VIEW_INPUT,-1,true,BACKSPACE);
    raw=BUTTON_LEFT; assert(!agents_menu_exit(ACTION_STD_CANCEL));
    raw=BUTTON_RC_LEFT; assert(!agents_menu_exit(ACTION_STD_CANCEL));
    raw=BUTTON_MENU; assert(agents_menu_exit(ACTION_STD_CANCEL));
    raw=BUTTON_RC_MENU; assert(agents_menu_exit(ACTION_STD_MENU));
    assert(!agents_menu_exit(0));
    agents_usb_exit(AGENTS_EXIT_OTHER);
    agents_usb_exit(AGENTS_SWITCH_WPS);
    agents_usb_exit(AGENTS_SWITCH_REMOTE_WPS);
    agents_usb_exit(AGENTS_SWITCH_COVERS);
    agents_usb_exit(AGENTS_EXIT_DEFERRED);
    assert(consent==0);
    agents_usb_exit(AGENTS_EXIT_MENU);
    assert(consent==1);
    puts("Agents: body/remote direction maps, phase routing, flat-list tabs, and MENU-only consent PASS");
}
'''
(OUT / 'config.h').write_text('')
path = OUT / 'agents.c'
path.write_text(prelude + '\n'.join(map(function, ('horizontal_button', 'agents_menu_exit',
                                                    'agents_usb_exit'))) + checks)
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I' + str(OUT), '-I' + str(ROOT), '-I' + str(ROOT / 'apps-ipod'),
                str(path), '-o', str(OUT / 'agents')], check=True)
subprocess.run([str(OUT / 'agents')], check=True, timeout=30)
