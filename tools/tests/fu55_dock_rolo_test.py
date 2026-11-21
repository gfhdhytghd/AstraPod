"""FU55: an Apple Universal Dock seated across a ROLO restart (the "Boot
changed, reboot now?" prompt after a Disk Mode install) is not recognised by
the new image until the iPod is re-seated; after a reset it is.

A reset leaves the accessory UART closed through the bootloader, and the dock
takes the line coming back as the player starting. ROLO kept the old image's
port open until the new image's serial_setup() closed it, and the new image's
first tick reopened it 10 ms later.

Host model: the production serial-6g.c accessory code (serial_acc_tick,
serial_setup, serial_accessory_release, tx_rdy, tx_writec) is compiled with
ASan/UBSan against recording UART/tick stubs, and the ROLO hold block is taken
from rolo.c's rolo_load(). Old image open -> ROLO -> new image open: the port
must stay closed (and the accessory supply off) for at least a second, the
tick must not reopen it meanwhile and nothing may be sent; the new image
opens it at its first tick as after a reset. Source contract: the hold runs
after a successful load and before interrupts go off; the post-eject prompt
is what calls rolo_load; the crumbs screen is shown after boot (USB, iAP
up) and no longer waits inside init(). FU55_SERIAL_SOURCE / FU55_ROLO_SOURCE
override the sources (the old ones fail). No device."""
from pathlib import Path
import os
import re
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
ser = Path(os.environ.get('FU55_SERIAL_SOURCE',
                          R / 'firmware/target/arm/s5l8702/ipod6g/serial-6g.c')).read_text()
rolo = Path(os.environ.get('FU55_ROLO_SOURCE', R / 'firmware/rolo.c')).read_text()


def fn(text, name):
    m = re.search(r"(?m)^[A-Za-z_][\w \t*]*\b" + name + r"\([^;{}]*\)\s*\{", text)
    if not m:
        return None
    end, depth = m.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


acc = ser[ser.index('#else /* IPOD_ACCESSORY_PROTOCOL */'):]
names = ['serial_bitrate', 'serial_acc_tick', 'serial_setup', 'serial_accessory_release']
parts = [fn(acc, n) for n in names]
release_present = parts[2] is not None
body = 'void serial_bitrate(int rate);\n' + '\n\n'.join(p for p in parts if p)
txs = '\n\n'.join(fn(ser, n) for n in ['tx_rdy', 'tx_writec'])
decl_end = acc.index('static void serial_acc_tick')
decls = acc[acc.index('static enum {'):decl_end]
decls = re.sub(r'(?s)#if defined\(HAVE_COMPOSITE_VIDEO_OUT\).*?#endif\n', '', decls)

load = fn(rolo, 'rolo_load')
assert load, 'rolo_load'
m = re.search(r'(?s)#if defined\(IPOD_6G\) && defined\(IPOD_ACCESSORY_PROTOCOL\)\n(.*?)#endif', load)
hold = m.group(1) if m else ''
off = re.search(r'#define ROLO_ACCESSORY_OFF \(([^)]*)\)', rolo)

src = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HZ 100
#define MAX(a,b) ((a)>(b)?(a):(b))
#define IF_IAP_MP(x)
#define IPOD_ACCESSORY_PROTOCOL 1
#define ULCON_DATA_BITS_8 0
#define ULCON_PARITY_NONE 0
#define ULCON_STOP_BITS_1 0
#define UCON_MODE_INTREQ 1
#define UCON_MODE_DISABLED 0
#define BRDATA_9600 1
#define BRDATA_19200 2
#define BRDATA_38400 3
#define BRDATA_57600 4
#define logf(...) ((void)0)
#define USEC_TIMER 0
struct uartc { int id; };
struct uartc_port { const struct uartc *uartc; int id; };
static const struct uartc the_uartc;
static struct uartc_port ser_port = { &the_uartc, 0 };
static long current_tick;
static bool line_open, supply = true, present = true;
static long line_closed_at = -1, longest_closed, sent, irq_level;
static void (*ticks[4])(void);
static int tick_add_task(void (*f)(void))
{ for (int i = 0; i < 4; i++) if (!ticks[i]) { ticks[i] = f; return 0; } assert(0); }
static int tick_remove_task(void (*f)(void))
{ for (int i = 0; i < 4; i++) if (ticks[i] == f) ticks[i] = 0; return 0; }
static void closed(void)
{ if (line_open) { line_open = false; line_closed_at = current_tick; } }
static void uartc_open(const struct uartc *u) { (void)u; closed(); }
static void uartc_close(const struct uartc *u) { (void)u; closed(); }
static void uartc_port_open(struct uartc_port *p)
{
    (void)p;
    if (!line_open && line_closed_at >= 0 && current_tick - line_closed_at > longest_closed)
        longest_closed = current_tick - line_closed_at;
    line_open = true;
}
static void uartc_port_close(struct uartc_port *p) { (void)p; closed(); }
static void uartc_port_config(struct uartc_port *p, int a, int b, int c) { (void)p; (void)a; (void)b; (void)c; }
static void uartc_port_set_tx_mode(struct uartc_port *p, int m) { (void)p; (void)m; }
static void uartc_port_set_rx_mode(struct uartc_port *p, int m) { (void)p; (void)m; }
static void uartc_port_abr_start(struct uartc_port *p) { (void)p; }
static void uartc_port_abr_stop(struct uartc_port *p) { (void)p; }
static void uartc_port_set_bitrate_raw(struct uartc_port *p, uint32_t b) { (void)p; (void)b; }
static bool uartc_port_tx_ready(struct uartc_port *p) { (void)p; return true; }
static void uartc_port_tx_byte(struct uartc_port *p, uint8_t c) { (void)p; (void)c; assert(line_open); sent++; }
static int pmu_accessory_present(void) { return present; }
static void iap_reset_state(void) { }
static int disable_irq_save(void) { return irq_level++; }
static void restore_irq(int l) { irq_level = l; }
static void accessory_supply_set(bool on) { supply = on; }
static long supply_off_at = -1, longest_supply_off;
static void run_ticks(long n)
{
    while (n--)
    {
        current_tick++;
        for (int i = 0; i < 4; i++) if (ticks[i]) ticks[i]();
        if (line_open && sent == 0) { }
    }
}
static void sleep(long n) { run_ticks(n); }
static volatile bool acc_plugged = false;
''' + decls + body + '\n' + txs + r'''
static void boot(void)          /* a fresh image: its statics as crt0 leaves them */
{
    memset(ticks, 0, sizeof ticks);
    acc_plugged = false; bitrate = 0; acc_absent_ticks = 0;
    supply = false;              /* pmu_preinit */
    serial_setup();
}
static void rolo_hold(void)
{
''' + hold + r'''
}
int main(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); fails++; } } while (0)
    boot(); supply = true;
    run_ticks(50);
    CHECK(line_open && acc_plugged);             /* old image, dock seated */
    long t0 = current_tick;
    rolo_hold();
    long held = current_tick - t0;
    CHECK(!line_open || !HAS_HOLD);
    CHECK(held >= HZ);                           /* about as long as a bootloader */
    CHECK(!supply);
    sent = 0; tx_writec(0x55); CHECK(sent == 0); CHECK(tx_rdy());
    /* the new image */
    boot();
    CHECK(!line_open);
    run_ticks(1);
    CHECK(line_open && acc_plugged);             /* opened at its first tick */
    CHECK(longest_closed >= HZ);                 /* the dock saw the player go */
    printf("held=%ld closed=%ld\n", held, longest_closed);
    printf("%s fails=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
'''
src = ('#define HAS_HOLD %d\n' % bool(hold)) + src
if off:
    src = '#define ROLO_ACCESSORY_OFF (%s)\n' % off.group(1) + src
(O / 'fu55_dock_rolo.c').write_text(src)
exe = O / 'fu55_dock_rolo'
subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                str(O / 'fu55_dock_rolo.c'), '-o', str(exe)], check=True)
r = subprocess.run([str(exe)], text=True, capture_output=True)
print(r.stdout + r.stderr, end='')
assert r.returncode == 0 and 'PASS' in r.stdout
assert release_present and hold, 'ROLO hold missing'

# Source contract
i_err = load.index('if (err <= 0)')
i_hold = load.index('serial_accessory_release();')
assert i_err < i_hold < load.index('disable_interrupt(IRQ_FIQ_STATUS)') < load.index('rolo_restart(')
assert hold.index('serial_accessory_release();') < hold.index('accessory_supply_set(false);') < hold.index('sleep(')
rel = fn(ser, 'serial_accessory_release')
assert 'tick_remove_task(serial_acc_tick)' in rel and 'acc_plugged = false' in rel
sd = (R / 'apps-ipod/system/shutdown.c').read_text()
cb = fn(sd, 'check_bootfile')
assert cb.index('LANG_BOOT_CHANGED') < cb.index('restart_firmware();')
restart = fn(sd, 'restart_firmware')
assert restart.index('audio_hard_stop();') < restart.index('usb_enable(false);')
assert restart.index('usb_enable(false);') < restart.index('rolo_load(BOOTDIR "/" BOOTFILE);')
assert restart.index('rolo_load(BOOTDIR "/" BOOTFILE);') < restart.index('usb_enable(true);')
assert 'usb_detect() == USB_INSERTED' in restart
assert 'check_bootfile(true)' in fn(sd, 'bootfile_session_end')
main = (R / 'apps-ipod/main.c').read_text()
mainf = main[main.index('int main(void)'):main.index('static void init(void) INIT_ATTR;')]
assert mainf.index('init();') < mainf.index('usb_start_monitoring();') < \
    mainf.index('crumbs_report_show()') < mainf.index('root_menu();')
assert 'default_event_handler(crumbs_ev)' in mainf
initf = fn(main, 'init')
assert initf.index('crumbs_report();') < initf.index('iap_setup(')
cr = (R / 'firmware/kernel/crumbs.c').read_text()
rep = fn(cr, 'crumbs_report')
assert 'button_get' not in rep and 'lcd_update' not in rep and 'crumbs-last.txt' in rep
show = fn(cr, 'crumbs_report_show')
assert 'button_get_w_tmo(' in show and 'return ev;' in show and 'lcd_setfont(font)' in show
print('fu55_dock_rolo_test: ok')
