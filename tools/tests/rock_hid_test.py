#!/usr/bin/env python3
"""Exercise the foreground HID lease with a simulated USB controller."""
from pathlib import Path
import subprocess
import sys


root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / "firmware/usb.c").read_text()
core = (root / "firmware/usbstack/usb_core.c").read_text()


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2]


stubs = {
    "config.h": "#define HAVE_USB_PLUGIN_HID 1\n",
    "system.h": """
static inline int disable_irq_save(void) { return 0; }
static inline void restore_irq(int level) { (void)level; }
""",
    "usb.h": """
#include "config.h"
#include <stdint.h>
enum { USB_EXTRACTED, USB_INSERTED, USB_POWERED };
enum { USB_DRIVER_MASS_STORAGE, USB_DRIVER_HID, USB_DRIVER_SERIAL,
       USB_DRIVER_IAP, USB_DRIVER_AUDIO, USB_NUM_DRIVERS };
int usb_detect(void);
void usb_signal_class_notify(int driver, uint32_t data);
""",
    "usb_core.h": "#include <stdbool.h>\nbool usb_core_plugin_hid_ready(void);\n",
}
for name, text in stubs.items():
    (out / name).write_text(text)

preamble = r'''
#include <assert.h>
#include <stdio.h>
#include "config.h"
#include "system.h"
#include "usb.h"
#include "usb_plugin_hid.h"
#include "usb_profile.h"
#include "usb_confirm.h"

#define USB_ENABLE_HID
#define USB_ENABLE_STORAGE
#define USB_ENABLE_SERIAL
#define USB_ENABLE_IAP
#define USB_ENABLE_AUDIO
#define HAVE_USB_CONFIRM
#define HAVE_USB_PODLINK
#define USB_MODE_CHARGE 1
#define NUM_CONFIGS 2

static bool usb_thread_entry = true;
static bool usb_host_probe_on, exclusive_storage_enabled;
static bool exclusive_storage_requested, podlink_requested, usb_car_no_audio;
static bool usb_hid = true, usb_iap = true;
static bool configured, resetting, transfer_busy;
static bool drivers[USB_NUM_DRIVERS];
static int cable, usb_state, usb_audio, usb_mode, notifications, reports;
static struct { int usb_audio; } global_settings = {1};
static struct usb_consent consent;
static bool usb_confirm_blocked(void) { return usb_consent_blocks(&consent); }
static bool usb_drive_only(void) { return false; }
static bool usb_iap_answer_iap2(void) { return false; }
static bool usb_audio_wanted(void) { return usb_audio != 0; }
static void usb_attach(void) {}
static void usb_iap2_host_new(void) {}
static bool usb_podlink_active(void) { return podlink_requested; }
static void usb_core_enable_driver(int driver, bool enable)
{
    drivers[driver] = enable;
}

int usb_detect(void) { return cable; }
bool usb_core_plugin_hid_ready(void)
{
    return configured && drivers[USB_DRIVER_HID] && !resetting;
}
void usb_signal_class_notify(int driver, uint32_t data)
{
    assert(driver == USB_DRIVER_HID && data == 0);
    ++notifications;
}
bool usb_hid_plugin_idle(void) { return !transfer_busy; }
void usb_hid_plugin_send(usage_page_t page, int id)
{
    assert(page == HID_USAGE_PAGE_KEYBOARD_KEYPAD ||
           page == HID_USAGE_PAGE_CONSUMER);
    assert(id > 0 && id <= UINT8_MAX);
    ++reports;
}
'''
tests = r'''
static void insert(void)
{
    cable = usb_state = USB_INSERTED;
    usb_plugin_hid_insert();
    usb_consent_begin(&consent, usb_plugin_hid_latched(), false);
    usb_configure_drivers(USB_POWERED);
}

static void detach(void)
{
    cable = usb_state = USB_EXTRACTED;
    configured = false;
    usb_plugin_hid_detach();
}

static bool send(void)
{
    return usb_plugin_hid_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD,
                               HID_KEYBOARD_PAGE_UP);
}

int main(void)
{
    /* An existing PodLink connection or disk owner cannot be displaced. */
    insert();
    assert(usb_podlink_open() && drivers[USB_DRIVER_SERIAL]);
    assert(!usb_plugin_hid_open());
    detach();
    exclusive_storage_requested = true;
    assert(!usb_plugin_hid_open());
    exclusive_storage_requested = false;
    usb_host_probe_on = true;
    assert(!usb_plugin_hid_open());
    usb_host_probe_on = false;
    podlink_requested = false;
    assert(usb_plugin_hid_open() && !usb_plugin_hid_open());
    assert(!usb_plugin_hid_latched() && !usb_podlink_open() && !send());
    usb_plugin_hid_close();
    assert(!usb_plugin_hid_reserved() && usb_podlink_open());

    assert(usb_plugin_hid_open());
    insert();
    assert(usb_plugin_hid_latched() && !usb_podlink_open());
    assert(offered_configs() == 1);
    for (int driver = 0; driver < USB_NUM_DRIVERS; ++driver)
        assert(drivers[driver] == (driver == USB_DRIVER_HID));
    usb_set_hid(false);
    assert(drivers[USB_DRIVER_HID]);
    assert(!usb_consent_observe(&consent, true, true, false));
    assert(!usb_consent_reply(&consent, consent.epoch, true));
    assert(!send());

    configured = true;
    assert(!usb_plugin_hid_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD, -1));
    assert(!usb_plugin_hid_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD, 256));
    assert(!usb_plugin_hid_send(HID_USAGE_PAGE_LEDS, 1));
    assert(send() && !send() && notifications == 1);
    usb_plugin_hid_deliver();
    usb_plugin_hid_deliver();
    assert(reports == 1);
    transfer_busy = true;
    assert(!send());
    transfer_busy = false;
    assert(usb_plugin_hid_send(HID_USAGE_PAGE_CONSUMER,
                               HID_CONSUMER_USAGE_PLAY_PAUSE));
    resetting = true;
    usb_plugin_hid_deliver();
    assert(!send() && reports == 1);
    resetting = false;

    assert(send());
    usb_plugin_hid_close();
    assert(usb_plugin_hid_latched() && !usb_plugin_hid_open());
    usb_plugin_hid_deliver();
    assert(!send() && reports == 1);

    detach();
    assert(!usb_plugin_hid_reserved() && usb_podlink_open());
    insert();
    assert(offered_configs() == 2 && drivers[USB_DRIVER_SERIAL]);
    assert(!drivers[USB_DRIVER_HID] && !send());
    detach();
    podlink_requested = false;
    assert(usb_plugin_hid_open());
    insert();
    configured = true;
    assert(send());
    detach();
    usb_plugin_hid_deliver();
    assert(!usb_plugin_hid_reserved() && !send() && reports == 1);
    puts("PASS HID-only drivers, lease refusal, queue, reset, exit and detach");
}
'''
harness = out / "usb_hid_harness.c"
harness.write_text("\n".join((
    preamble,
    function(source, "static inline void usb_configure_drivers(int for_state)"),
    function(source, "void usb_set_hid(bool enable)"),
    function(source, "bool usb_plugin_hid_open(void)"),
    function(source, "bool usb_podlink_open(void)"),
    function(core, "static int offered_configs(void)"),
    tests,
)))
program = out / "usb_hid_harness"
subprocess.run([
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
    "-D_USB_CLASS_DRIVER_H_",
    "-I", str(out), "-I", str(root / "firmware/usbstack"),
    "-I", str(root / "firmware/export"),
    str(harness), str(root / "firmware/usbstack/usb_plugin_hid.c"),
    "-o", str(program),
], check=True)
subprocess.run([str(program.resolve())], check=True)

hid = (root / "firmware/usbstack/usb_hid.c").read_text()
driver_preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "usb_hid_usage_tables.h"

#define HAVE_USB_PLUGIN_HID 1
#define HID_BUF_SIZE_MSG 16
#define HID_BUF_SIZE_CMD 16
#define HID_NUM_BUFFERS 5
#define HID_BUF_INC(i) do { (i) = ((i) + 1) % HID_NUM_BUFFERS; } while (0)
#define USB_DEVBSS_ATTR
#define USB_DIR_IN 0x80
#define EP_IN 1
#define logf(...)
#define buf_dump(...)
#define REPORT_ID_COUNT 3
#define BUF_LEN_KEYBOARD 7
#define BUF_LEN_CONSUMER 4

typedef struct {
    uint8_t usage_page;
    uint8_t (*buf_set)(unsigned char *buf, int id);
    bool is_key_released;
} usb_hid_report_t;

static uint8_t buf_set_keyboard(unsigned char *buf, int id);
static uint8_t buf_set_consumer(unsigned char *buf, int id);
static usb_hid_report_t usb_hid_reports[REPORT_ID_COUNT] = {
    {0}, {HID_USAGE_PAGE_KEYBOARD_KEYPAD, buf_set_keyboard, true},
    {HID_USAGE_PAGE_CONSUMER, buf_set_consumer, true},
};

static bool active, currently_sending, failing;
static unsigned int cur_buf_send, cur_buf_prepare;
static unsigned char send_buffer[HID_NUM_BUFFERS][HID_BUF_SIZE_MSG];
static size_t send_buffer_len[HID_NUM_BUFFERS];
static unsigned char captured[16][HID_BUF_SIZE_MSG];
static unsigned int sent;

static int usb_drv_send_nonblocking(int ep, const void *bytes, int length)
{
    assert(ep == EP_IN);
    if (failing)
        return -1;
    assert(sent < 16 && length <= HID_BUF_SIZE_MSG);
    memcpy(captured[sent++], bytes, length);
    return 0;
}

static void usb_plugin_hid_flush(void) {}
static void usb_hid_try_send_drv(void);
'''

driver_tests = r'''
static void complete(void)
{
    usb_hid_transfer_complete(EP_IN, USB_DIR_IN, 0, 0);
}

int main(void)
{
    usb_hid_init_connection();
    assert(usb_hid_plugin_idle());
    usb_hid_plugin_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD, HID_KEYBOARD_PAGE_UP);
    assert(sent == 1 && captured[0][0] == 1 &&
           captured[0][2] == HID_KEYBOARD_PAGE_UP && !usb_hid_plugin_idle());
    complete();
    assert(sent == 2 && captured[1][0] == 1 && captured[1][2] == 0 &&
           !usb_hid_plugin_idle());
    complete();
    assert(usb_hid_plugin_idle());

    usb_hid_send(HID_USAGE_PAGE_CONSUMER, HID_CONSUMER_USAGE_VOLUME_INCREMENT);
    usb_hid_send(HID_USAGE_PAGE_CONSUMER, HID_CONSUMER_USAGE_PLAY_PAUSE);
    usb_hid_send(HID_USAGE_PAGE_CONSUMER, HID_CONSUMER_USAGE_VOLUME_DECREMENT);
    for (int i = 0; i < 4; ++i)
        complete();
    assert(sent == 6 && captured[2][0] == 2 && captured[4][0] == 2 &&
           captured[2][1] == HID_CONSUMER_USAGE_VOLUME_INCREMENT &&
           captured[3][1] == 0 &&
           captured[4][1] == HID_CONSUMER_USAGE_PLAY_PAUSE &&
           captured[5][1] == 0 && usb_hid_plugin_idle());

    failing = true;
    usb_hid_plugin_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD, HID_KEYBOARD_PAGE_UP);
    assert(usb_hid_plugin_idle() && sent == 6);
    failing = false;
    usb_hid_plugin_send(HID_USAGE_PAGE_KEYBOARD_KEYPAD, HID_KEYBOARD_PAGE_UP);
    assert(sent == 7 && !usb_hid_plugin_idle());
    usb_hid_disconnect();
    assert(!usb_hid_plugin_idle());
    usb_hid_init_connection();
    assert(usb_hid_plugin_idle() && sent == 7);
    puts("PASS HID keyboard and consumer press/release, queue capacity and reset");
}
'''

driver_harness = out / "usb_hid_driver_harness.c"
driver_harness.write_text("\n".join((
    driver_preamble,
    *(function(hid, signature) for signature in (
        "static uint8_t buf_set_keyboard(unsigned char *buf, int id)\n{",
        "static uint8_t buf_set_consumer(unsigned char *buf, int id)\n{",
        "static void usb_hid_reset_queue(void)",
        "static int usb_hid_init_connection(void)",
        "static void usb_hid_disconnect(void)",
        "static void usb_hid_transfer_complete(int ep, int dir, int status, int length)",
        "static void usb_hid_try_send_drv(void)\n{",
        "static void usb_hid_queue(unsigned char *data, int length)",
        "void usb_hid_send(usage_page_t usage_page, int id)",
        "bool usb_hid_plugin_idle(void)",
        "void usb_hid_plugin_send(usage_page_t usage_page, int id)",
    )),
    driver_tests,
)))
driver_program = out / "usb_hid_driver_harness"
subprocess.run([
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
    "-I", str(root / "firmware/usbstack"),
    str(driver_harness), "-o", str(driver_program),
], check=True)
subprocess.run([str(driver_program.resolve())], check=True)
