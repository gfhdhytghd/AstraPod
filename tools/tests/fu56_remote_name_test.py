"""FU56: the host names its remote-music backend (Rufin or Cider).

Checks, on the production sources (no hardware):
 1. cider_model.c, compiled with ASan/UBSan: cider_remote_parse() accepts only
    "CF<TAB>gen<TAB>remote<TAB>Name" of the current generation (Rufin -> 3,
    any other name -> 2, else 0); a CA of a new generation forgets the name.
 2. cider.c's publish_status(), cider_remote_name(), queue_control() and
    cider_remote_home(), compiled against stubs: the status word carries the
    name lock-free; Home row entry queues "remote-home" only for a host that
    named its backend, and only while available.
 3. Source contract: the ci capability list stays at 8 tokens (older hosts
    refuse 9) with remote-name in place of modes5 ("loading" implies the
    19/20-field CN); user-facing strings use the name; debug strings stay.
"""
from pathlib import Path
import re, subprocess, sys
from podlink_rust_build import wire_args

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
P = R / 'apps-ipod/screens/playback'
C = (P / 'cider.c').read_text()
CC = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
      '-fno-pie', '-no-pie', '-I' + str(R / 'apps-ipod'), '-I' + str(R / 'firmware/export')]


def fn(s, name):
    m = re.search(r'(?m)^(?:static[ \t]+)?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{', s)
    assert m, name
    e = m.end(); d = 1
    while d:
        d += (s[e] == '{') - (s[e] == '}'); e += 1
    return s[m.start():e]


def run(name, source, extra=()):
    (O / (name + '.c')).write_text(source)
    subprocess.run(CC + [str(O / (name + '.c')), *extra, '-o', str(O / name)], check=True)
    out = subprocess.run([str(O / name)], check=True, capture_output=True, text=True).stdout
    print(out, end='')
    return out


# --- 1. parse and reset -----------------------------------------------------
model = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "screens/playback/cider_model.h"
static unsigned char p(const char *s, uint32_t g) { return cider_remote_parse(s, strlen(s), g); }
int main(void)
{
    assert(p("CF\t7\tremote\tRufin", 7) == 3);
    assert(p("CF\t7\tremote\tCider", 7) == 2);
    assert(p("CF\t7\tremote\tOther", 7) == 2);
    assert(p("CF\t7\tremote\tRufinX", 7) == 2);
    assert(p("CF\t7\tremote\tRufin", 8) == 0);     /* other generation */
    assert(p("CF\t7\tremote\t", 7) == 0);          /* no name */
    assert(p("CF\t7\tlocalstore", 7) == 0);
    assert(p("CF\t7\tremote", 7) == 0);
    assert(p("CF\t4294967295\tremote\tRufin", 4294967295u) == 3);
    struct cider_state s; memset(&s, 0, sizeof s);
    const char ca[] = "CA\t7\t1\t";
    assert(cider_state_apply(&s, ca, sizeof ca - 1, 1) == 1 && s.generation == 7);
    s.remote = 3;
    assert(cider_state_apply(&s, ca, sizeof ca - 1, 2) == 1 && s.remote == 3);   /* same session keeps it */
    const char ca2[] = "CA\t8\t1\t";
    assert(cider_state_apply(&s, ca2, sizeof ca2 - 1, 3) == 1 && s.remote == 0); /* new session: Cider */
    printf("parse/reset PASS (struct cider_state %zu bytes)\n", sizeof s);
    return 0;
}
'''
run('model', model, [str(P / 'cider_model.c'), *wire_args()])

# --- 2. status word and Home row control ------------------------------------
ctrl = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "screens/playback/cider_model.h"
#define ARRAYLEN(a) ((int)(sizeof(a)/sizeof((a)[0])))
static int cider_mutex;
static void mutex_lock(int *m) { (void)m; }
static void mutex_unlock(int *m) { (void)m; }
static struct cider_state state;
static volatile uint32_t status_word;
static char commands[12][384];
static unsigned int ncommands;
static uint32_t next_request;
''' + fn(C, 'publish_status') + '\n' + fn(C, 'cider_remote_name') + '\n' + fn(C, 'queue_control') + '\n' + \
    fn(C, 'cider_remote_home') + r'''
int main(void)
{
    state.generation = 9; state.available = true;
    publish_status(); assert(!strcmp(cider_remote_name(), "Cider"));
    state.remote = 3; publish_status(); assert(!strcmp(cider_remote_name(), "Rufin"));
    state.remote = 2; publish_status(); assert(!strcmp(cider_remote_name(), "Cider"));
    state.remote = 0; cider_remote_home(); assert(ncommands == 0);     /* old host: nothing */
    state.remote = 3; cider_remote_home();
    assert(ncommands == 1 && !strcmp(commands[0], "cc\t9\t1\tremote-home\t0"));
    state.available = false; cider_remote_home(); assert(ncommands == 1);
    state.available = true; state.volume = 100; state.repeat = 4; state.shuffle = 1; state.remote = 3;
    publish_status();
    assert(((status_word >> 4) & 127) == 100 && ((status_word >> 11) & 1) == 1 &&
           ((status_word >> 12) & 7) == 4 && (status_word >> 15) == 1);  /* no overlap */
    printf("status word / remote-home PASS\n");
    return 0;
}
'''
run('control', ctrl)

# --- 3. source contract -----------------------------------------------------
hello = re.search(r'"ci\\t1\\t%d\\t([^"]+)"', C)
tokens = hello[1].split(',')
assert len(tokens) == 8 and 'remote-name' in tokens and 'modes5' not in tokens and 'loading' in tokens, tokens
assert 'cider_remote_parse((const char *)d, n, state.generation)' in fn(C, 'store_text')
recv = fn(C, 'receive')
assert re.search(r'unsigned char remote = state.remote;\s*store_text\(m->data, m->length\);\s*'
                 r'if \(remote != state.remote\) button_queue_post\(BUTTON_REDRAW, 0\);', recv)
root = (R / 'apps-ipod/root_menu.c').read_text()
assert 'MENUITEM_RETURNVALUE_DYNTEXT(cider_item, GO_TO_CIDER, item_callback,' in root
assert '"%s Remote", cider_remote_name()' in root and 'MENUITEM_RETURNVALUE(cider_item' not in root
browser = (P / 'cider_browser.c').read_text()
assert 'if (!depth && !show_covers) cider_remote_home();' in fn(browser, 'cider_browse')
assert 'depth ? "%s%s" : "%s Remote%s"' in browser and '"Cider Remote"' not in browser
assert '"%s could not play the selected item", cider_remote_name()' in browser
assert 'root_count < (int)ARRAYLEN(root_rows) - 1' in browser
assert '"%s play failed", cider_remote_name()' in (P / 'cider_wps.c').read_text()
controls = (P / 'cider_controls.c').read_text()
assert '"%s did not answer", cider_remote_name()' in controls
assert '"Cider cover store"' in controls                      # debug strings stay "Cider"
assert 'cider_active() ? cider_remote_name() :' in (R / 'apps-ipod/skin/skin_tokens.c').read_text()
assert 'static inline const char *cider_remote_name(void) { return "Cider"; }' in (P / 'cider.h').read_text()
print('source contract PASS')
print('FU56 remote name PASS')
