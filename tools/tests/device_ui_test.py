#!/usr/bin/env python3
"""Host regressions for idle-list battery updates; no display/device access."""
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(os.environ.get('ART_TEST_ROOT', Path(__file__).resolve().parents[2]))
OUT = Path(tempfile.mkdtemp(prefix='device-ui-tests-'))
os.environ['ART_TEST_ROOT'] = str(ROOT)
os.environ['ART_TEST_OUT'] = str(OUT)
sys.path.insert(0, str(Path(__file__).with_name('albumart')))
from support import build, function, run  # noqa: E402

code = r'''
struct gui_synclist { int unused; };
static bool inhibited;
static int dirty, flushes, action_result, submits;
#define ACTION_NONE 0
static bool list_render_motion_due(struct gui_synclist *l){(void)l;return false;}
static void gui_synclist_draw(struct gui_synclist *l){(void)l;assert(0);}
static void lcd_update(void){submits++;}
static int list_do_action_timeout(struct gui_synclist *l, int t)
{ (void)l; return t; }
static void gui_synclist_keyclick_callback(void) {}
static void keyclick_set_callback(void (*f)(void), void *d)
{ (void)f; (void)d; }
static bool ui_flush_inhibited(void) { return inhibited; }
static void ui_inhibit_flush(bool value) { inhibited = value; }
static int get_action(int c, int t)
{
    (void)c; (void)t;
    assert(inhibited);
    dirty++; /* ACTIONUPDATE painted the battery in the framebuffer. */
    return action_result;
}
static bool gui_synclist_do_button(struct gui_synclist *l, int *a)
{ (void)l; return *a != 0; }
static void ui_flush_dirty(void)
{ assert(!inhibited); if (dirty) { flushes++; dirty = 0; } }
'''
listing = (ROOT / 'apps-ipod/widgets/list.c').read_text()
start = listing.index('bool list_do_action(')
code += listing[start:listing.index('\n}', start) + 2]
code += r'''
enum { NO_CHARGER, CHARGER_UNPLUGGED, CHARGER_PLUGGED, CHARGER };
enum { DISCHARGING, CHARGING };
enum { SYS_CHARGER_CONNECTED, SYS_CHARGER_DISCONNECTED };
static int charger_input_state, charge_state, charge_done_count;
static int last_sent_battery_level, broadcasts;
static bool charge_finished, pin_charging;
#define POWER_INPUT_CHARGER 2
#define CHARGE_DONE_SAMPLES 4
static bool charging_state(void) { return pin_charging; }
static void queue_broadcast(int e, int d)
{ (void)e; (void)d; broadcasts++; }
'''
code += function('firmware/powermgmt.c', 'detect_charger')
power = (ROOT / 'firmware/powermgmt.c').read_text()
code = code.replace('#define CHARGE_DONE_SAMPLES 4',
                    re.search(r'^#define CHARGE_DONE_SAMPLES\s+\d+',
                              power, re.M).group())
start = power.index('static inline void charging_algorithm_step',
                    power.index('#elif CONFIG_CHARGING == CHARGING_MONITOR'))
code += power[start:power.index('static inline void charging_algorithm_close', start)]
code += r'''
int main(void)
{
    struct gui_synclist list = {0};
    int a = -1;
    action_result = 0;
    assert(!list_do_action(0, 10, &list, &a));
    assert(a == 0 && flushes == 1 && submits == 1 && !dirty && !inhibited);
    action_result = 1;
    assert(list_do_action(0, 10, &list, &a));
    assert(flushes == 2 && submits == 1 && !dirty);
    inhibited = true;
    action_result = 0;
    assert(!list_do_action(0, 10, &list, &a));
    assert(inhibited && dirty && flushes == 2 && submits == 2);
    inhibited = false;
    ui_flush_dirty();
    assert(flushes == 3);
    /* A nearly full/full battery with !CHRG high still advertises the
     * charging source; only the percentage cap follows this pin. */
    for (int active = 0; active < 2; active++)
    {
        charger_input_state = NO_CHARGER;
        charge_finished = false;
        charge_done_count = broadcasts = 0;
        pin_charging = active != 0;
        assert(detect_charger(POWER_INPUT_CHARGER));
        charging_algorithm_step();
        assert(charge_state == CHARGING);
        assert(detect_charger(POWER_INPUT_CHARGER));
        assert(charger_input_state == CHARGER && broadcasts == 1);
        for (int i = 0; i < CHARGE_DONE_SAMPLES; i++)
            charging_algorithm_step();
        assert(charge_state == CHARGING);
        assert(charge_finished == !pin_charging);
        assert(detect_charger(0));
        charging_algorithm_step();
        assert(charge_state == DISCHARGING && !charge_finished);
    }
    puts("PASS: idle status flush, handled action, outer inhibition, "
         "charging-source transitions and full-battery termination");
}
'''
exe = build('device-ui', code)
print(run([exe], 'device-ui'), end='')

# The native Chrome splits the time, background activity and battery cells.
chrome = 'apps-ipod/ui/native_chrome.c'
source = (ROOT / chrome).read_text()
layout_type = source[source.index('struct status_layout {'):source.index('static struct status_layout status_layout(')]
code = r'''
enum { UI_METRIC_STATUS_ICON_SIZE, UI_METRIC_STATUS_ICON_GAP, UI_METRIC_STATUS_GROUP_GAP,
       UI_METRIC_STATUS_PAD_LEFT, UI_METRIC_STATUS_PAD_RIGHT };
struct ui_theme { uint16_t metric[5]; };
struct native_chrome { const struct ui_theme *theme; };
''' + layout_type + function(chrome, 'status_layout') + r'''
int main(void)
{
    struct ui_theme theme = {{18, 4, 6, 6, 6}};
    struct native_chrome chrome = {&theme};
    for (int percent = 0; percent <= 38; percent += 38)
    {
        struct status_layout p = status_layout(&chrome, 320, 46, percent);
        assert(p.battery_x + 18 <= 320);
        assert(p.play_x + 18 <= p.battery_x);
        assert(p.clock_x + p.clock_width <= p.play_x);
        assert(2*p.clock_x + p.clock_width == 320);
        assert(p.busy_x + 18 <= p.clock_x);
        assert(6 + p.title_width <= p.busy_x);
        if (percent)
            assert(p.play_x + 18 <= p.percent_x && p.percent_x + p.percent_width <= p.battery_x);
    }
    puts("PASS: native battery, playback, busy and clock cells do not overlap");
}
'''
print(run([build('native-chrome-layout', code)], 'native-chrome-layout'), end='')
print('Logs:', OUT)
