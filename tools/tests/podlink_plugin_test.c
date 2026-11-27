/* Exercise the actual plugin Shell and LCD entry points without a device. */
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define __CONFIG_H__
#define IPOD_6G
#define IPOD_ACCESSORY_PROTOCOL
#define HAVE_USB_PODLINK
#define ASTRAPOD_ROCK_COMPAT
#include "podlink-plugin-test.h"
#include "podlink-shell-settings.h"
#include "podlink-shell-playback.h"
#include "podlink-shell-lcd.h"
#include "button_usb_remote.h"
#include "button-target.h"
#include "sm1-test-control.h"
#include "usb_ch9.h"

#define ARRAYLEN(array) (sizeof(array) / sizeof((array)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define IF_PRIO(...)
#define IF_COP(...)
#define TIME_BEFORE(a, b) ((long)((a) - (b)) < 0)
#define HZ 100
#define TIMEOUT_BLOCK -1
#define DEFAULT_STACK_SIZE 1024
#define SYS_PODLINK_PLUGIN_TEST 9
#define BUTTON_NONE 0
#define PLUGIN_ERROR -1
#define PLUGIN_USB_CONNECTED 0x100
#define PLUGIN_DIR "/.rockbox/rocks"
#define EP_NUM(ep) ((ep) & 15)
#define EP_CONTROL 0
#define EP_DIR(ep) (((ep) & USB_DIR_IN) != 0)
#define DIR_IN 1
#define CONFIGURED 2
#define USB_CONTROL_ACK 1
#define USB_CONTROL_STALL 2
#define logf(...)
#define remote_log_event(...)

static unsigned long current_tick;
static int irq_depth;
static int disable_irq_save(void) { return irq_depth++; }
static void restore_irq(int level) { assert(level == --irq_depth); }

static long posted;
static int posted_data;
static bool button_queue_try_post(long button, int data)
{
    if (posted)
        return false;
    posted = button;
    posted_data = data;
    return true;
}

static int usb_state = CONFIGURED;
static bool resident = true, exclusive, bus_reset_pending, control_cancelled;
static bool usb_podlink_active(void) { return resident; }
static bool usb_exclusive_storage(void) { return exclusive; }
static uint8_t usb_control_data[256];
static int response;
static size_t response_size;
static void usb_core_control_response(int code, const void *payload, size_t size)
{
    assert(size == 0 || payload == usb_control_data);
    response = code;
    response_size = size;
}
void shell_settings_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed)
{
    (void)req;
    (void)buffer;
    (void)capacity;
    (void)allowed;
    assert(0);
}
void shell_playback_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed)
{
    (void)req;
    (void)buffer;
    (void)capacity;
    (void)allowed;
    assert(0);
}
void shell_lcd_request(const struct usb_ctrlrequest *req, uint8_t *buffer,
                       bool allowed)
{
    (void)req;
    (void)buffer;
    (void)allowed;
    assert(0);
}
static void usb_core_control_request_handler(struct usb_ctrlrequest *req,
                                            uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    assert(req->bRequest == USB_REMOTE_KEY_REQUEST);
    usb_core_control_response(usb_state == CONFIGURED && button_usb_remote_request(req) ?
                              USB_CONTROL_ACK : USB_CONTROL_STALL, NULL, 0);
}

struct usb_class_driver
{
    void (*transfer_complete)(int, int, int, int);
};
static struct { struct usb_class_driver *driver; } ep_data[16][2];
struct usb_transfer_completion_event_data
{
    int ep, status, length;
    struct usb_ctrlrequest *req;
};

static int request(unsigned type, unsigned code, unsigned value, unsigned len,
                   const void *payload, int actual)
{
    assert(len <= sizeof(usb_control_data));
    if (payload)
        memcpy(usb_control_data, payload, len);
    struct usb_ctrlrequest req = {
        .bRequestType = type, .bRequest = code,
        .wIndex = code == SM1_TEST_USB_REQUEST ? SM1_TEST_USB_INDEX : actual,
        .wValue = value, .wLength = len,
    };
    struct usb_transfer_completion_event_data event = {
        .req = &req, .ep = EP_CONTROL, .length = actual,
    };
    response = 0;
    extern void usb_core_handle_transfer_completion(
        struct usb_transfer_completion_event_data *event);
    usb_core_handle_transfer_completion(&event);
    assert(response == USB_CONTROL_ACK || response == USB_CONTROL_STALL);
    if (response == USB_CONTROL_ACK)
        assert(response_size == ((type & USB_DIR_IN) ? len : 0));
    return response;
}

#define SHELL_IN(value, len) request(0xc0, SM1_TEST_USB_REQUEST, value, len, NULL, 0)
#define KEY(value, edge) request(0x40, USB_REMOTE_KEY_REQUEST, value, 0, NULL, edge)
static int shell_out(unsigned value, const void *payload, unsigned len)
{
    return request(0x40, SM1_TEST_USB_REQUEST, value, len, payload, len);
}

static unsigned read16(unsigned offset)
{
    return usb_control_data[offset] | (unsigned)usb_control_data[offset + 1] << 8;
}

static uint32_t read32(unsigned offset)
{
    return read16(offset) | (uint32_t)read16(offset + 2) << 16;
}

static void expect_status(unsigned seq, unsigned state, unsigned reason,
                          int result, const char *name)
{
    assert(SHELL_IN(3, 52) == USB_CONTROL_ACK);
    assert(read32(0) == 1 && read32(4) == seq && read32(8) == state);
    assert(read32(12) == reason && (int32_t)read32(16) == result);
    assert(strcmp((char *)usb_control_data + 20, name) == 0);
}

static int wanted_result;
static const char *wanted_path;
static int rock_run_from_shell(const char *path)
{
    assert(strcmp(path, wanted_path) == 0);
    if (wanted_result < 0)
        return wanted_result; /* A loader failure never reaches the entry point. */

    const char *name = strrchr(path, '/') + 1;
    assert(strncmp(name, "maze.", 5) == 0);
    assert(podlink_plugin_test_claim(true));
    podlink_plugin_test_started();
    expect_status(wanted_result == PLUGIN_USB_CONNECTED ? 3 : 1, 2, 0, 0, "maze");
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    assert(KEY(USB_REMOTE_KEY_SELECT, 1) == USB_CONTROL_ACK);
    assert(KEY(USB_REMOTE_KEY_LEFT, 1) == USB_CONTROL_ACK);
    assert(KEY(USB_REMOTE_KEY_RIGHT, 1) == USB_CONTROL_ACK);
    assert(KEY(USB_REMOTE_KEY_PLAY, 1) == USB_CONTROL_ACK);
    assert(button_usb_remote_read() == (BUTTON_SELECT | BUTTON_LEFT |
                                       BUTTON_RIGHT | BUTTON_PLAY));
    assert(KEY(USB_REMOTE_KEY_VOL_UP, 1) == USB_CONTROL_ACK);
    assert(posted == BUTTON_SCROLL_BACK && (uint32_t)posted_data == 0x81000000);
    posted = 0;
    assert(KEY(USB_REMOTE_KEY_VOL_DOWN, 1) == USB_CONTROL_ACK);
    assert(posted == BUTTON_SCROLL_FWD);
    posted = 0;
    assert(KEY(USB_REMOTE_KEY_VOL_DOWN, 2) == USB_CONTROL_ACK && !posted);

    assert(shell_out(10, NULL, 0) == USB_CONTROL_ACK);
    assert(button_usb_remote_read() & BUTTON_MENU);
    current_tick += HZ / 20;
    assert(!(button_usb_remote_read() & BUTTON_MENU));

    if (wanted_result == PLUGIN_USB_CONNECTED)
    {
        podlink_plugin_test_usb_taken();
        assert(!button_usb_remote_read());
        exclusive = true;
        assert(SHELL_IN(3, 52) == USB_CONTROL_STALL);
    }
    podlink_plugin_test_stopped();
    podlink_plugin_test_release();
    assert(!button_usb_remote_read());
    return wanted_result;
}

struct semaphore { bool signalled; };
static jmp_buf worker_stop;
static void (*worker)(void);
static void semaphore_init(struct semaphore *sem, int max, int count)
{ assert(max == 1 && count == 0); sem->signalled = false; }
static void semaphore_release(struct semaphore *sem) { sem->signalled = true; }
static void semaphore_wait(struct semaphore *sem, int timeout)
{
    assert(timeout == TIMEOUT_BLOCK);
    if (!sem->signalled)
        longjmp(worker_stop, 1);
    sem->signalled = false;
}
static unsigned create_thread(void (*entry)(void), void *stack, size_t stack_size,
                              unsigned flags, const char *name)
{
    (void)stack; (void)stack_size; (void)flags; (void)name;
    worker = entry;
    return 1;
}
static void run_capture(void) { if (!setjmp(worker_stop)) worker(); }

#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define RGB565 1
#define LCD_PIXELFORMAT RGB565
typedef uint16_t fb_data;
static fb_data framebuffer[LCD_HEIGHT][LCD_WIDTH], lcd_dblbuf[LCD_HEIGHT][LCD_WIDTH];
#define FRAMEBUFFER_SIZE sizeof(framebuffer)
#define FBADDR(x, y) (&framebuffer[(y)][(x)])
static fb_data capture_storage[2][LCD_HEIGHT][LCD_WIDTH];
static bool alloc_fails, alloc_live, alloc_pinned;

static int core_alloc_free_space(size_t size)
{
    assert(size == sizeof(capture_storage) && !alloc_live);
    if (alloc_fails)
        return -1;
    alloc_live = true;
    return 1;
}

static void *core_get_data_pinned(int handle)
{
    assert(handle == 1 && alloc_live && !alloc_pinned);
    alloc_pinned = true;
    return capture_storage;
}

static void core_put_data_pinned(void *data)
{
    assert(data == capture_storage && alloc_pinned);
    alloc_pinned = false;
}

static int core_free(int handle)
{
    assert(handle == 1 && alloc_live && !alloc_pinned);
    alloc_live = false;
    return 0;
}

static bool lcd_ispowered = true, held;
static int lcd_dma_ch;
static struct { struct { bool thread; } blocker; } lcd_mutex;
static bool dmac_ch_running(int *ch) { return *ch != 0; }
static void mutex_lock(void *mutex)
{ assert(mutex == &lcd_mutex && !lcd_mutex.blocker.thread); lcd_mutex.blocker.thread = true; }
static void mutex_unlock(void *mutex)
{ assert(mutex == &lcd_mutex && lcd_mutex.blocker.thread); lcd_mutex.blocker.thread = false; }
static bool lcd_updates_held(void) { return held; }
static bool is_thread_context(void) { return true; }
static bool irq_enabled(void) { return irq_depth == 0; }
static void displaylcd_setup(int x, int y, int width, int height)
{ assert(x >= 0 && y >= 0 && width > 0 && height > 0); }
static void displaylcd_dma(int width, int height)
{ assert(lcd_mutex.blocker.thread && width > 0 && height > 0); lcd_dma_ch = 1; }
static void displaylcd_wait_dma(void)
{ assert(lcd_mutex.blocker.thread); lcd_dma_ch = 0; }
static void lcd_write_yuv420_lines(const unsigned char *const src[3],
                                   uint16_t *dst, int width, int stride)
{
    (void)src; (void)stride;
    for (int i = 0; i < 2 * width; i++)
        dst[i] = 0x07e0;
}

#include "production.inc"

static void test_commands(void)
{
    expect_status(0, 0, 0, 0, "");
    assert(SHELL_IN(3, 51) == USB_CONTROL_STALL);
    assert(shell_out(10, NULL, 0) == USB_CONTROL_STALL);
    assert(shell_out(9, NULL, 0) == USB_CONTROL_STALL);
    assert(shell_out(9, "Maze", 4) == USB_CONTROL_STALL);
    assert(shell_out(9, "maze.rock", 9) == USB_CONTROL_STALL);
    assert(shell_out(9, "games/maze", 10) == USB_CONTROL_STALL);
    assert(shell_out(9, "maze\0", 5) == USB_CONTROL_STALL);
    char long_name[32] = {0};
    assert(shell_out(9, long_name, sizeof(long_name)) == USB_CONTROL_STALL);
    assert(request(0x40, SM1_TEST_USB_REQUEST, 9, 4, "maze", 3) == USB_CONTROL_STALL);
    assert(request(0x41, SM1_TEST_USB_REQUEST, 9, 4, "maze", 4) == USB_CONTROL_STALL);
    control_cancelled = true;
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    control_cancelled = false;
    usb_state = 1;
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    usb_state = CONFIGURED;
    resident = false;
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    resident = true;
    exclusive = true;
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    exclusive = false;
    assert(podlink_plugin_test_claim(false));
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    podlink_plugin_test_release();
    posted = BUTTON_LEFT;
    assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
    posted = 0;
    expect_status(0, 0, 0, 0, "");

    static const struct {
        const char *name, *path;
        int result;
        unsigned reason;
    } cases[] = {
        {"maze", PLUGIN_DIR "/games/maze.rock", 0, 1},
        {"rockblox1d", PLUGIN_DIR "/games/rockblox1d.rock", -27, 3},
        {"maze", PLUGIN_DIR "/games/maze.rock", PLUGIN_USB_CONNECTED, 2},
    };
    for (unsigned i = 0; i < ARRAYLEN(cases); i++)
    {
        assert(shell_out(9, cases[i].name, strlen(cases[i].name)) == USB_CONTROL_ACK);
        assert(posted == SYS_PODLINK_PLUGIN_TEST);
        expect_status(i + 1, 1, 0, 0, cases[i].name);
        assert(shell_out(9, "maze", 4) == USB_CONTROL_STALL);
        posted = 0;
        wanted_path = cases[i].path;
        wanted_result = cases[i].result;
        assert(podlink_plugin_test_launch() == (wanted_result == PLUGIN_USB_CONNECTED));
        exclusive = false; /* Resident USB reconnects after Disk Mode. */
        expect_status(i + 1, 0, cases[i].reason, wanted_result, cases[i].name);
    }
    assert(shell_out(9, "maze", 4) == USB_CONTROL_ACK);
    assert(podlink_plugin_test_claim(false));
    posted = 0;
    assert(!podlink_plugin_test_launch());
    expect_status(4, 0, 3, PLUGIN_ERROR, "maze");
    podlink_plugin_test_release();
    assert(KEY(USB_REMOTE_KEY_LEFT, 1) == USB_CONTROL_ACK && !button_usb_remote_read());
    assert(KEY(USB_REMOTE_KEY_SELECT, 1) == USB_CONTROL_ACK);
    assert(button_usb_remote_read() == BUTTON_RC_SELECT);
    puts("PASS: EP0 shape, allowlist, physical test keys, signed results and Disk Mode");
}

static void snapshot_info(unsigned seq, unsigned state)
{
    assert(SHELL_IN(4, 16) == USB_CONTROL_ACK);
    assert(read32(0) == seq && read32(4) == FRAMEBUFFER_SIZE);
    assert(read16(8) == 320 && read16(10) == 240 && read16(12) == 256);
    assert(read16(14) == state);
}

static unsigned pixel(unsigned x, unsigned y)
{
    unsigned offset = (y * LCD_WIDTH + x) * sizeof(fb_data);
    assert(SHELL_IN(0x100 + offset / 256, 256) == USB_CONTROL_ACK);
    return read16(offset % 256);
}

static void test_lcd_no_memory(void)
{
    alloc_fails = true;
    podlink_plugin_test_lcd_init();
    assert(!worker && !alloc_live);
    snapshot_info(0, 0);
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    assert(shell_out(11, NULL, 0) == USB_CONTROL_STALL);
    assert(SHELL_IN(0x100, 256) == USB_CONTROL_STALL);
    alloc_fails = false;
    puts("PASS: LCD updates and USB status when snapshot allocation fails");
}

static void test_lcd(void)
{
    memset(capture_storage, 0xff, sizeof(capture_storage));
    podlink_plugin_test_lcd_init();
    assert(alloc_live && alloc_pinned);
    snapshot_info(0, 0);
    assert(SHELL_IN(0x100, 256) == USB_CONTROL_STALL);
    assert(shell_out(11, "x", 1) == USB_CONTROL_STALL);
    assert(SHELL_IN(4, 15) == USB_CONTROL_STALL);

    framebuffer[0][0] = 0xf800;
    framebuffer[239][319] = 0x001f;
    lcd_update_rect(0, 0, 320, 240);
    framebuffer[24][127] = 0x07e0;
    framebuffer[25][128] = 0x1234;
    lcd_update_rect(127, 24, 2, 2);
    assert(shell_out(11, NULL, 0) == USB_CONTROL_ACK);
    snapshot_info(1, 1);
    assert(SHELL_IN(0x100, 256) == USB_CONTROL_STALL);
    assert(shell_out(11, NULL, 0) == USB_CONTROL_STALL);
    run_capture();
    snapshot_info(1, 2);
    assert(pixel(0, 0) == 0xf800 && pixel(319, 239) == 0x001f);
    assert(pixel(127, 24) == 0x07e0 && pixel(128, 25) == 0x1234);
    assert(SHELL_IN(0x100, 255) == USB_CONTROL_STALL);
    assert(SHELL_IN(0x100 + 600, 256) == USB_CONTROL_STALL);

    held = true;
    static fb_data scene[LCD_HEIGHT][LCD_WIDTH];
    scene[0][0] = 0x001f;
    assert(lcd_present_scene(scene[0]));
    lcd_update_rect(0, 0, 320, 240); /* Held UI framebuffer stays off-panel. */
    assert(pixel(0, 0) == 0xf800);
    assert(shell_out(11, NULL, 0) == USB_CONTROL_ACK);
    run_capture();
    snapshot_info(2, 2);
    assert(pixel(0, 0) == 0x001f && pixel(319, 239) == 0);
    assert(pixel(127, 24) == 0 && pixel(128, 25) == 0);

    unsigned char yuv[8] = {0};
    unsigned char *planes[] = {yuv, yuv, yuv};
    lcd_blit_yuv(planes, 0, 0, 2, 70, 80, 2, 2);
    assert(shell_out(11, NULL, 0) == USB_CONTROL_ACK);
    run_capture();
    assert(pixel(70, 80) == 0x07e0 && pixel(71, 81) == 0x07e0);
    assert(pixel(72, 81) == 0 && pixel(0, 0) == 0x001f);
    puts("PASS: stable raw, native and YUV LCD snapshots with RGB565 byte order");
}

int main(void)
{
    test_commands();
    test_lcd_no_memory();
    test_lcd();
}
