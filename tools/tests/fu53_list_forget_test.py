"""FU53: no list outlives its screen in the skin redraw callback.

Boot died on a NULL read in skinlist_get_line_count (list->parent[0] == NULL)
under the FU51 no-access page 0. The list was a dead stack frame: do_menu's
Agents shortcut leaves after list_do_action_timeout() (which arms the oneshot
GUI_EVENT_NEED_UI_UPDATE callback with current_lists = &lists) without
gui_synclist_do_button() (which clears it). The next full status bar refresh
drew current_lists.

Checks, on the production sources:
 1. list.c's callback, gui_synclist_forget() and gui_synclist_draw()'s guard,
    compiled with ASan against recording stubs: arm -> forget -> event fires
    draws nothing and the event is removed. Forgetting another list keeps
    the armed one and its scroll generation. A list with no parent is not
    drawn. Reusing a list's address leaves the old scroll inactive.
 2. Every caller of list_do_action_timeout() either calls
    gui_synclist_do_button() right after its get_action(), or calls
    gui_synclist_forget() on the same list before returning; do_menu forgets
    after its loop and before anything that can refresh the status bar.
No hardware.
"""
from pathlib import Path
import re, subprocess, sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
L = (R / 'apps-ipod/widgets/list.c').read_text()
S = (R / 'apps-ipod/widgets/list_skinned.c').read_text()


def fn(s, name):
    m = re.search(r'(?m)^(?:static[ \t]+)?(?:int|bool|void)[ \t*]+' + name +
                  r'\([^;{}]*\)\s*\{', s)
    assert m, name
    e = m.end(); d = 1
    while d:
        d += (s[e] == '{') - (s[e] == '}'); e += 1
    return s[m.start():e]


# --- 1. behaviour -----------------------------------------------------------
cb = fn(L, '_lists_uiviewport_update_callback')
forget = fn(L, 'gui_synclist_forget')
draw = fn(L, 'gui_synclist_draw')
scroll_active = fn(S, 'scroll_active')
scroll_cancel = fn(S, 'skinlist_scroll_cancel')
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define UNLIKELY(x) (x)
#define SCREEN_MAIN 0
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for (int i = 0; i < NB_SCREENS; i++)
#define GUI_EVENT_NEED_UI_UPDATE 7
struct viewport { int x, y, width, height; };
struct screen { int screen_type; } screens[1];
struct gui_synclist {
    int nb_items;
    struct viewport *parent[NB_SCREENS];
    unsigned generation;
};
static struct { unsigned generation; } scroll;
static struct gui_synclist *current_lists;
typedef void handler_t(unsigned short, void *, void *);
static handler_t *armed; static int removed, drawn, laid_out, flushed;
static struct gui_synclist *last_drawn;
static void remove_event_ex(unsigned short id, handler_t *h, void *ud)
{ assert(id == GUI_EVENT_NEED_UI_UPDATE && ud == NULL); if (armed == h) armed = NULL; removed++; }
static void skin_flush_dirty(void) { flushed++; }
static void gui_synclist_layout(struct gui_synclist *l) { (void)l; laid_out++; }
static bool skinlist_draw(struct screen *d, struct gui_synclist *l)
{ (void)d; assert(l->parent[0] && l->parent[0]->height == 99); drawn++; last_drawn = l; return true; }
static void list_draw(struct screen *d, struct gui_synclist *l) { (void)d; (void)l; assert(0); }
static bool native_menu_draw(struct gui_synclist *l) { (void)l; return false; }
void gui_synclist_draw(struct gui_synclist *gui_list);
@CB@
@SCROLL_ACTIVE@
@SCROLL_CANCEL@
@FORGET@
@DRAW@
static void arm(struct gui_synclist *l)   /* what list_do_action_timeout does */
{ armed = _lists_uiviewport_update_callback; current_lists = l; }
static void fire(void)
{ handler_t *h = armed; armed = NULL; if (h) h(GUI_EVENT_NEED_UI_UPDATE, NULL, NULL); }
int main(void)
{
    struct viewport vp = { 0, 0, 10, 99 };
    struct gui_synclist a = { 3, { &vp }, 1 };
    struct gui_synclist b = { 2, { &vp }, 2 };
    struct gui_synclist z = { 0, { NULL }, 3 };
    arm(&a); fire();
    assert(drawn == 1 && last_drawn == &a && flushed == 1);  /* the live list */
    scroll.generation = a.generation;
    arm(&a); gui_synclist_forget(&b);                 /* another list: kept */
    assert(armed && current_lists == &a && removed == 0);
    assert(scroll_active(&a) && !scroll_active(&b));
    gui_synclist_forget(&a);                          /* screen leaves */
    assert(!armed && current_lists == NULL && removed == 1);
    assert(scroll.generation == 0);
    fire(); assert(drawn == 1);                       /* nothing drawn */
    scroll.generation = a.generation;
    a.generation = 4;                                /* same address, new list */
    assert(!scroll_active(&a) && !skinlist_scroll_cancel(&a));
    arm(&a); current_lists = NULL; armed = _lists_uiviewport_update_callback;
    fire(); assert(drawn == 1 && flushed == 1);       /* cleared: no draw */
    gui_synclist_draw(&z); assert(drawn == 1 && laid_out == 1);  /* no parent */
    gui_synclist_draw(&b); assert(drawn == 2 && laid_out == 2);
    puts("behaviour ok");
    return 0;
}
'''.replace('@CB@', cb).replace('@FORGET@', forget).replace('@DRAW@', draw).replace(
    '@SCROLL_ACTIVE@', scroll_active).replace('@SCROLL_CANCEL@', scroll_cancel)
c = O / 'fu53_list.c'; c.write_text(harness)
exe = O / 'fu53_list'
subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-g', str(c), '-o', str(exe)], check=True)
print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())

# --- 2. every list_do_action_timeout caller ---------------------------------
callers = []
for p in sorted((R / 'apps-ipod').rglob('*.c')):
    s = p.read_text(errors='replace')
    for m in re.finditer(r'list_do_action_timeout\(\s*&?([\w.>-]+)', s):
        if p.relative_to(R).as_posix() == 'apps-ipod/widgets/list.c':
            continue           # the definition, and list_do_action() (do_button)
        line = s.count('\n', 0, m.start()) + 1
        nxt = s[m.end():m.end() + 400]
        follows = re.match(r'[^;]*;\s*(?:oldbutton = button;\s*)?'
                           r'(?:if\s*\(\s*)?(?:LIKELY\()?gui_synclist_do_button\(\s*&?'
                           + re.escape(m.group(1)), nxt)
        callers.append((str(p.relative_to(R)), line, m.group(1),
                        'do_button follows' if follows else 'needs forget'))
need = [c for c in callers if c[3] == 'needs forget']
assert [(f, v) for f, _, v, _ in need] == [('apps-ipod/widgets/menu.c', 'lists')], callers
menu = fn((R / 'apps-ipod/widgets/menu.c').read_text(), 'do_menu')
loop_end = menu.rindex('\n    }\n', 0, menu.index('gui_synclist_forget(&lists);'))
assert menu.index('while (!done)') < loop_end
tail = menu[menu.index('gui_synclist_forget(&lists);'):]
assert tail.index('gui_synclist_forget') < tail.index('viewportmanager_theme_undo')
assert len(re.findall(r'(?m)^\s*return\b', menu)) == 1 and 'return ret;' in tail
assert 'agents_nav_pending() != AGENTS_NAV_NONE' in menu
for f, line, v, how in callers:
    print(f'{f}:{line} {v}: {how}')
print('fu53_list_forget_test: ok')
