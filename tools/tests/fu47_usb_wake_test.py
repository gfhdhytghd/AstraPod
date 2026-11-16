"""FU47: usb_thread waits without a timeout outside Disk Mode (as FU39) and
polls at HZ/20 only while the FU42 body-MENU hold, 0x5c flush/3 s timeout or
detach/restore deadline is live. Production usb.c text, RAM stubs, no device."""
from pathlib import Path
import re
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
usb = (R/'firmware/usb.c').read_text()


def fn(text, name):
    m = re.search(r"(?m)^[A-Za-z_][\w \t*]*\b" + name + r"\([^;{}]*\)\s*\{", text)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


state = usb[usb.index('static long disk_menu_at'):
            usb.index('\n#endif\n#endif\n#ifdef USB_ENABLE_AUDIO', usb.index('static long disk_menu_at'))]
thread = fn(usb, 'usb_thread')
head = thread[thread.index('#ifdef HAVE_USB_PODLINK'):thread.index('#else')]
head = head.replace('#ifdef HAVE_USB_PODLINK', '')
assert 'queue_wait(&usb_queue, &ev)' in head and 'usb_disk_needs_poll()' in head
# FU39 shape: the generic wait has no timeout.
assert 'queue_wait_w_tmo(&usb_queue, &ev, MAX(1, HZ / 20));\n#else' not in thread

src = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define HZ 100
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TIME_BEFORE(a,b) ((long)(a)-(long)(b)<0)
#define BUTTON_MENU 0x100
#define BUTTON_RC_MENU 0x800
enum { USB_EXTRACTED, USB_INSERTED };
enum { USB_CONSENT_CHARGE = 3, SYS_USB_DISCONNECTED = 77 };
struct queue_event { long id; intptr_t data; };
struct event_queue { int n; };
static struct event_queue usb_queue;
static long current_tick;
static bool exclusive_storage_enabled, exclusive_storage_requested;
static bool usb_disk_ejected, podlink_requested, cable = true;
static int usb_num_acks_to_expect, raw_buttons, attached = 1, timed, untimed, pending;
static struct { int state; } usb_consent;
static int button_status(void) { return raw_buttons; }
static int usb_detect(void) { return cable ? USB_INSERTED : USB_EXTRACTED; }
static int disable_irq_save(void) { return 0; }
static void restore_irq(int n) { (void)n; }
static void sleep(int t) { current_tick += t; }
static void usb_stack_enable(bool on) { attached = on; }
static void usb_slave_mode(bool on) { (void)on; }
static int queue_broadcast(long id, intptr_t d) { (void)id; (void)d; return 1; }
static void usb_core_storage_ejected(void) {}
static void usb_consent_begin(void *c, bool a, bool b) { (void)c; (void)a; (void)b; }
static void usb_configure_drivers(int s) { (void)s; podlink_requested = true; }
#define USB_ENABLE_STORAGE
#define HAVE_USB_PODLINK
static void usb_release_exclusive_storage(void);
static void usb_storage_ejected(void);
@STATE@
@POLL@
@NEEDS@
@RELEASE@
@EJECTED@
/* Infinite wait: the test supplies one event or the thread sleeps forever. */
static void queue_wait(struct event_queue *q, struct queue_event *ev)
{ (void)q; untimed++; ev->id = pending ? 1 : -1; pending = 0; }
static void queue_wait_w_tmo(struct event_queue *q, struct queue_event *ev, int t)
{ (void)q; assert(t == HZ / 20); timed++; current_tick += t; ev->id = -2; }
static void pass(void) { struct queue_event ev; @HEAD@ (void)ev; }
static void enter_disk(void)
{ exclusive_storage_requested = exclusive_storage_enabled = true; podlink_requested = false; }
static void idle_window(const char *what)
{
    int t = timed, u = untimed; pass();
    assert(timed == t && untimed == u + 1);  /* no event: thread stays asleep */
    for (int i = 0; i < 50; i++) { pending = 1; pass(); }
    assert(timed == t);
    printf("%s: 0 timed wakes over 51 passes\n", what);
}
int main(void)
{
    idle_window("PodLink/menus (no Disk Mode)");
    /* Body MENU 1 s -> 0x5c pending -> host ack -> detach + PodLink restore. */
    enter_disk(); int t0 = timed;
    pass(); assert(timed == t0 + 1);
    raw_buttons = BUTTON_MENU;
    while (!disk_exit_pending) { pass(); assert(timed - t0 < 40); }
    raw_buttons = 0;
    unsigned char status[8]; usb_disk_status(status); assert(status[1] && status[2]);
    assert(usb_disk_flush_ack(disk_exit_request));
    while (exclusive_storage_enabled || disk_return_at) { pass(); assert(timed - t0 < 60); }
    assert(attached && podlink_requested && !usb_disk_take_warning());
    printf("body MENU hold + host ack: %d timed polls, then restore\n", timed - t0);
    idle_window("after ack restore");
    /* No ack: 3 s timeout warning, detach anyway. */
    enter_disk(); t0 = timed; raw_buttons = BUTTON_MENU;
    while (!disk_exit_pending) pass();
    raw_buttons = 0; long start = current_tick;
    while (exclusive_storage_enabled || disk_return_at) { pass(); assert(timed - t0 < 120); }
    assert(current_tick - start >= 3 * HZ && usb_disk_take_warning() && podlink_requested);
    printf("3 s flush timeout: %d timed polls, warning, restore\n", timed - t0);
    idle_window("after timeout restore");
    /* Remote MENU: host unmounts + ejects (SCSI eject -> usb_storage_ejected). */
    enter_disk(); t0 = timed; raw_buttons = BUTTON_RC_MENU;
    for (int i = 0; i < 40; i++) pass();
    assert(!disk_exit_pending);          /* remote bit is the host's job */
    raw_buttons = 0; usb_storage_ejected(); assert(disk_return_at);
    while (disk_return_at) { pass(); assert(timed - t0 < 60); }
    assert(attached && podlink_requested && usb_disk_ejected);
    printf("host eject: %d timed polls, restore\n", timed - t0);
    idle_window("after host eject");
    /* Unplug in Disk Mode cancels the hold; extracted cable never polls. */
    enter_disk(); raw_buttons = BUTTON_MENU; pass(); cable = false;
    exclusive_storage_requested = exclusive_storage_enabled = false;
    pass(); assert(!disk_exit_pending && !disk_return_at && !disk_menu_down);
    raw_buttons = 0; idle_window("unplugged");
    puts("FU47 usb_thread wake budget PASS");
    return 0;
}
'''
for k, v in {'STATE': state, 'POLL': fn(usb, 'usb_disk_poll'),
             'NEEDS': fn(usb, 'usb_disk_needs_poll'),
             'RELEASE': fn(usb, 'usb_release_exclusive_storage'),
             'EJECTED': fn(usb, 'usb_storage_ejected'), 'HEAD': head}.items():
    src = src.replace('@' + k + '@', v)
src = src.replace('void usb_release_exclusive_storage(void)\n{',
                  'static void usb_release_exclusive_storage(void)\n{', 1)
src = src.replace('void usb_storage_ejected(void)\n{', 'static void usb_storage_ejected(void)\n{', 1)
src = src.replace('usb_consent_begin(&usb_consent, false, false);',
                  'usb_consent_begin(&usb_consent, false, false);', 1)
c = O/'fu47_usb_wake.c'; c.write_text(src)
exe = O/'fu47_usb_wake'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-Wno-unused-variable', '-g', '-fsanitize=address,undefined',
                str(c), '-o', str(exe)], check=True)
out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
print(out, end='')
assert 'FU47 usb_thread wake budget PASS' in out
