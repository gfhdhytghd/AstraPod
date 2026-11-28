"""FU52: EP0 OUT DMA only ever targets _ep0_buffer. The production
usb-designware.c EP0 path (epstart/transfer/ep0_recv/xfer_complete/
setup_received/oepint and the FU51 OUT DMA guard) is compiled for a 32-bit
host against the real register header, with a small DMA-mode controller
model: the status stage, 8/100-byte data stages, a SETUP that arrives
while the status stage is armed, and (FU55) SET_REPORT data stages of
1..512 bytes with the spurious StatusRecvd after them and a SETUP mid stage. FU52_DW_SOURCE overrides the source file
(used to show the old code fails); FU52_NO_GUARD=1 drops the FU51 guard.
No device."""
from pathlib import Path
import os
import re
import subprocess
import sys

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1]).resolve(); O.mkdir(parents=True, exist_ok=True)
dw = Path(os.environ.get('FU52_DW_SOURCE', R / 'firmware/drivers/usb-designware.c')).read_text()


def fn(text, name):
    m = re.search(r"(?m)^[A-Za-z_][\w \t*]*\b" + name + r"\([^;{}]*\)\s*\{", text)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


# Use the device init's exact DAINTMSK setup against the real register map.
mask_end = dw.index("    usb_endpoints = DWC_DAINTMSK;")
mask_start = dw.rindex("    /*", 0, mask_end)
mask_setup = dw[mask_start:mask_end + len("    usb_endpoints = DWC_DAINTMSK;")]

decls = dw[dw.index('#define USB_DW_NUM_DIRS'):dw.index('static uint32_t usb_endpoints;')]
names = ['complete_transfer', 'usb_dw_get_ep', 'usb_dw_maxpktsize', 'usb_dw_maxxfersize',
         'usb_dw_calc_packets', 'usb_dw_check_dma', 'usb_dw_epstart', 'usb_dw_transfer',
         'usb_dw_ep0_recv', 'usb_dw_handle_xfer_complete', 'usb_dw_handle_setup_received',
         'usb_dw_oepint']
body = '\n\n'.join(fn(dw, n) for n in names)
body = body.replace('extern char _icodeend[];', '')

inc = O / 'inc'; inc.mkdir(exist_ok=True)
for h in ('config.h', 'cpu.h'):
    (inc / h).write_text('')
# copy the real register header next to the empty config/cpu stubs
(inc / 'usb-designware.h').write_text((R / 'firmware/export/usb-designware.h').read_text())

src = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t regs[0x1000];
#define OTGBASE ((uint32_t)(uintptr_t)regs)
#define USB_NUM_ENDPOINTS 7
#include "usb-designware.h"
#define USB_DEVBSS_ATTR
#define USB_DW_PHYSADDR(x) (x)
#define CACHEALIGN_SIZE 32
#define CACHEALIGN_DOWN(x) ((x) & ~31u)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TTB_BASE_ADDR 0xfffff000u
#define DRAM_ORIG 0u
#define DRAM_SIZE 0xfffff000u
#define EP_CONTROL 0
#define USB_DIR_OUT 0
#define USB_DIR_IN 0x80
#define ISO_LOG_MAX 8
#define logf(...) ((void)0)
#define usb_log(...) ((void)0)
#define DISCARD_DCACHE_RANGE(b,s) ((void)(b),(void)(s))
#define COMMIT_DCACHE_RANGE(b,s) ((void)(b),(void)(s))
enum { CS_USB_DMA_OUT, CS_USB_DMA_IN, CS_USB_STAGE, CR_USB_DMA_BAD, CR_USB_SETUP, USB_STAGE_SETUP };
struct semaphore { int n; };
struct usb_ctrlrequest { uint8_t bRequestType, bRequest; uint16_t wValue, wIndex, wLength; } __attribute__((packed));
static char *_icodeend = (char *)0x1000;   /* core IRAM code ends here */
static void semaphore_release(struct semaphore *s) { s->n++; }
static void crumb_status(int k, uint32_t v) { (void)k; (void)v; }
static void crumb(int k, uint32_t v) { (void)k; (void)v; }
static jmp_buf panic_jmp; static int panics; static char panic_msg[128];
static void panicf(const char *f, ...)
{ va_list a; va_start(a, f); vsnprintf(panic_msg, sizeof panic_msg, f, a); va_end(a); panics++; longjmp(panic_jmp, 1); }
static int usb_drv_port_speed(void) { return 1; }
static int usb_drv_get_frame_number(void) { return 0; }
static int usb_dw_log_ep(int e, int d) { return e | d << 7; }
static void usb_dw_iso_in_armed(int e) { (void)e; }
static uint32_t hw_maxbytes = 0x7ffff, hw_maxpackets = 0x3ff, iso_in_odd;
static uint8_t iso_starts_logged[USB_NUM_ENDPOINTS][2], iso_errors_logged[USB_NUM_ENDPOINTS][2];
static bool address_status;
static int completions, last_status, last_length, setups, cancels;
static struct usb_ctrlrequest last_req;
static void usb_core_transfer_complete(int ep, int dir, int status, int length)
{ assert(ep == 0 || ep == 1); (void)dir; completions++; last_status = status; last_length = length; }
static void usb_core_control_cancelled(void) { cancels++; }
static void usb_core_setup_received(struct usb_ctrlrequest *r) { setups++; last_req = *r; }
static bool handle_set_address(struct usb_ctrlrequest *r) { (void)r; return false; }
''' + decls + r'''
static void usb_dw_flush_endpoint(int epnum, enum usb_dw_epdir epdir)
{
    struct usb_dw_ep *e = &usb_dw_ep_list[epnum][epdir];
    e->busy = false; e->status = -1;
    DWC_EPCTL(epnum, epdir) &= ~EPENA;
}
''' + body + r'''

/* ---- DMA-mode controller model for EP0 OUT ---- */
static int low_writes;  /* DMA that would have hit the vector page */
static void dma_write(const void *p, uint32_t n)
{
    uint32_t a = DWC_DOEPDMA(0);
    if (!n) return;
    if (a < 0x1000) { low_writes++; return; }
    memcpy((void *)(uintptr_t)a, p, n);
    DWC_DOEPDMA(0) = a + n;
}
static void host_out(const uint8_t *p, uint32_t n)
{
    assert(DWC_DOEPCTL(0) & EPENA);
    uint32_t t = DWC_DOEPTSIZ(0);
    assert(n <= (t & 0x7ffff));
    dma_write(p, n);
    DWC_DOEPTSIZ(0) = (t & ~0x7ffffu & ~(0x3ffu << 19)) | (((t >> 19) & 0x3ff) - 1) << 19 | ((t & 0x7ffff) - n);
    DWC_DOEPCTL(0) &= ~EPENA;
    DWC_DOEPINT(0) = XFRC;
    usb_dw_oepint(0);
}
static void host_setup(const struct usb_ctrlrequest *r)
{
    /* The core ACKs a SETUP whatever the endpoint state and DMAs it to
     * DOEPDMA. */
    dma_write(r, 8);
    DWC_DOEPCTL(0) &= ~EPENA;
    DWC_DOEPINT(0) = STUP;
    usb_dw_oepint(0);
}
static void arm_out(int ep, void *buf, uint32_t n)
{
    struct usb_dw_ep *e = usb_dw_get_ep(ep, USB_DW_EPDIR_OUT);
    e->active = true;
    if (!setjmp(panic_jmp)) assert(usb_dw_transfer(ep, USB_DW_EPDIR_OUT, buf, n) == 0);
}
#define EP0_PHYS ((uint32_t)(uintptr_t)&_ep0_buffer[0])
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); fails++; } } while (0)
static int fails;

static void status_stage(void)
{
    completions = 0;
    arm_out(0, NULL, 0);
    CHECK(panics == 0);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS);
    CHECK((DWC_DOEPTSIZ(0) >> 29) & 3);         /* SETUP still accepted */
    host_out(NULL, 0);                           /* status ZLP */
    CHECK(low_writes == 0);
    CHECK(completions == 1 && last_status == 0 && last_length == 0);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS);           /* re-armed for SETUP */
}

static void setup_during_status(void)
{
    setups = 0;
    arm_out(0, NULL, 0);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS);
    struct usb_ctrlrequest r = { 0xc0, 0x5c, 0x1234, 0, 8 };
    host_setup(&r);
    CHECK(low_writes == 0);
    CHECK(memcmp(_ep0_buffer, &r, 8) == 0);
    CHECK(setups == 1 && memcmp(&last_req, &r, 8) == 0);
    CHECK(!usb_dw_get_ep(0, USB_DW_EPDIR_OUT)->busy);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS && (DWC_DOEPCTL(0) & EPENA));
    /* a second one right behind it (host retry) */
    r.wValue = 0x5678; host_setup(&r);
    CHECK(setups == 2 && last_req.wValue == 0x5678 && low_writes == 0);
}

static void data_stage(uint32_t n, int offset)
{
    static uint8_t mem[600];
    static uint8_t data[512];
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + n);
    memset(mem, 0xee, sizeof mem);
    uint8_t *buf = mem + 16 + offset;
    completions = 0;
    arm_out(0, buf, n);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS);
    for (uint32_t done = 0; done < n; ) {
        uint32_t c = MIN(64u, n - done);
        CHECK(DWC_DOEPDMA(0) == EP0_PHYS && (DWC_DOEPCTL(0) & EPENA));
        CHECK(completions == 0);
        host_out(data + done, c);
        done += c;
    }
    CHECK(low_writes == 0);
    CHECK(completions == 1 && last_status == 0 && last_length == (int)n);
    CHECK(memcmp(buf, data, n) == 0);
    for (uint8_t *p = mem; p < mem + sizeof mem; p++)
        if (p < buf || p >= buf + n) CHECK(*p == 0xee);
}

/* FU55: SET_REPORT data stages as an iAP accessory sends them (usb_iap.c
 * hands usb_core's 256-byte usb_control_data to SET_REPORT; the driver is
 * tested to 512): every length, single and multi-packet, wLength not a
 * multiple of 64, unaligned buffers; then the controller's spurious
 * StatusRecvd|XferCompl on the re-armed EP0 once the host moves to the
 * status stage; then a SETUP in the middle of a data stage. */
static void status_recvd(void)
{
    uint32_t t = DWC_DOEPTSIZ(0);
    CHECK(DWC_DOEPCTL(0) & EPENA);               /* re-armed for SETUP */
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS && (t & 0x7ffff) == 64);
    DWC_DOEPCTL(0) &= ~EPENA;
    DWC_DOEPINT(0) = XFRC | STATUSRECVD;
    usb_dw_oepint(0);
    CHECK(DWC_DOEPCTL(0) & EPENA);               /* and again after it */
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS);
}

static void set_report_sweep(void)
{
    for (uint32_t n = 1; n <= 512; n++)
        for (int off = 0; off < 2; off++)
        {
            int before = fails;
            data_stage(n, off * 3);
            int done = completions;
            status_recvd();
            CHECK(completions == done);          /* no second completion */
            if (fails != before) { printf("  at n=%u off=%d\n", n, off * 3); return; }
        }
}

static void setup_mid_data(void)
{
    static uint8_t buf[256];
    uint8_t data[64];
    memset(data, 0x5a, sizeof data);
    completions = cancels = setups = 0;
    arm_out(0, buf, 200);
    host_out(data, 64);                          /* first packet of 200 */
    CHECK(completions == 0 && usb_dw_get_ep(0, USB_DW_EPDIR_OUT)->busy);
    struct usb_ctrlrequest r = { 0x21, 0x09, 0x0209, 2, 100 };   /* SET_REPORT */
    host_setup(&r);
    CHECK(cancels == 1 && setups == 1 && memcmp(&last_req, &r, 8) == 0);
    CHECK(!usb_dw_get_ep(0, USB_DW_EPDIR_OUT)->busy && completions == 0);
    CHECK(DWC_DOEPDMA(0) == EP0_PHYS && low_writes == 0);
    data_stage(100, 1);                          /* the new request's data */
}

static void guard(void)
{
    static uint8_t ok[64] __attribute__((aligned(32)));
#ifndef HAVE_CRUMBS
    return;
#endif
    panics = 0;
    DWC_DOEPCTL(1) = 512;                        /* configured bulk OUT */
    arm_out(1, NULL, 64);                        /* other OUT DMA to 0 */
    CHECK(panics == 1 && strstr(panic_msg, "OUT1 DMA to 00000000"));
    panics = 0;
    arm_out(1, ok, 64);
    CHECK(panics == 0 && DWC_DOEPDMA(1) == (uint32_t)(uintptr_t)ok);
}

static void test_endpoint_irq_mask(void)
{
    uint32_t usb_endpoints;
''' + mask_setup + r'''
    /* S5L8702 has hardware status bits above its last serviced endpoint. */
    DWC_DAINT = (1u << 8) | (1u << 23);
    assert(!(DWC_DAINT & DWC_DAINTMSK));
    DWC_DAINT = 1u | (1u << 22);
    assert((DWC_DAINT & DWC_DAINTMSK) == DWC_DAINT);
    assert(usb_endpoints == DWC_DAINTMSK);
}

int main(void)
{
    test_endpoint_irq_mask();
    ep0_buffer = &_ep0_buffer[0];
    usb_dw_get_ep(0, USB_DW_EPDIR_OUT)->active = true;
    usb_dw_ep0_recv();
    status_stage();
    setup_during_status();
    data_stage(8, 0);
    data_stage(8, 3);                            /* unaligned caller buffer */
    data_stage(64, 0);
    data_stage(100, 0);
    data_stage(100, 1);
    status_stage();
    set_report_sweep();
    setup_mid_data();
    status_stage();
    guard();
    printf("%s fails=%d low_writes=%d\n", fails ? "FAIL" : "PASS", fails, low_writes);
    return fails != 0;
}
'''
# FU52_NO_GUARD=1 builds without the FU51 DMA guard (pre-FU51 behaviour).
if not os.environ.get('FU52_NO_GUARD'):
    src = '#define HAVE_CRUMBS 1\n' + src
(O / 'fu52_ep0_dma.c').write_text(src)
exe = O / 'fu52_ep0_dma'
subprocess.run(['gcc', '-m32', '-std=gnu11', '-O1', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                '-Wno-unused-variable', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-sanitize=shift',
                '-I', str(inc),
                str(O / 'fu52_ep0_dma.c'), '-o', str(exe)], check=True)
r = subprocess.run([str(exe)], text=True, capture_output=True)
print(r.stdout + r.stderr, end='')
assert r.returncode == 0 and 'PASS' in r.stdout
print('fu52_ep0_dma_test: ok')
