/* Snapshot the target's presented RGB565 pixels. GNU GPL version 2 or later. */
#include "podlink-plugin-test.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"
#include <string.h>

enum { CAPTURE_EMPTY, CAPTURE_QUEUED, CAPTURE_READY };

static uint16_t presented[LCD_HEIGHT][LCD_WIDTH];
static uint16_t snapshot[LCD_HEIGHT][LCD_WIDTH];
static uint32_t generation;
static unsigned state;
static struct semaphore capture_pending;
static bool capture_available;
static uint32_t capture_stack[DEFAULT_STACK_SIZE / sizeof(uint32_t)];

_Static_assert(LCD_WIDTH == 320 && LCD_HEIGHT == 240 && LCD_PIXELFORMAT == RGB565,
               "PodLink screenshot pixel format");

struct capture_info
{
    uint32_t generation;
    uint32_t bytes;
    uint16_t width, height, chunk_size, state;
};
_Static_assert(sizeof(struct capture_info) == 16, "screenshot info wire size");

/* The LCD driver's presentation lock serializes these two calls. The input
 * rows are packed as sent to the panel, including partial and YUV updates. */
void podlink_plugin_test_lcd_present(const uint16_t *pixels, int x, int y,
                                    int width, int height)
{
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        width > LCD_WIDTH - x || height > LCD_HEIGHT - y)
        return;
    if (width == LCD_WIDTH)
        memcpy(presented[y], pixels, width * height * sizeof(*pixels));
    else
        for (int row = 0; row < height; row++)
            memcpy(&presented[y + row][x], pixels + row * width,
                   width * sizeof(*pixels));
}

void podlink_plugin_test_lcd_copy(uint16_t *pixels)
{
    memcpy(pixels, presented, sizeof(presented));
}

static void capture_thread(void)
{
    for (;;)
    {
        semaphore_wait(&capture_pending, TIMEOUT_BLOCK);
        lcd_copy_presented_for_test(snapshot[0]);
        int level = disable_irq_save();
        state = CAPTURE_READY;
        restore_irq(level);
    }
}

void podlink_plugin_test_lcd_init(void)
{
    semaphore_init(&capture_pending, 1, 0);
    capture_available = create_thread(capture_thread, capture_stack,
                                      sizeof(capture_stack), 0, "lcd snapshot"
                                      IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU)) != 0;
}

bool podlink_plugin_test_lcd_begin(void)
{
    int level = disable_irq_save();
    bool accepted = capture_available && state != CAPTURE_QUEUED;
    if (accepted)
    {
        generation++;
        state = CAPTURE_QUEUED;
        semaphore_release(&capture_pending);
    }
    restore_irq(level);
    return accepted;
}

size_t podlink_plugin_test_lcd_read(unsigned value, unsigned length, uint8_t *reply)
{
    if (value == 4 && length == sizeof(struct capture_info))
    {
        int level = disable_irq_save();
        const struct capture_info info = {
            generation, sizeof(snapshot), LCD_WIDTH, LCD_HEIGHT,
            PODLINK_TEST_CHUNK_SIZE, state
        };
        memcpy(reply, &info, sizeof(info));
        restore_irq(level);
        return sizeof(info);
    }

    if (value < PODLINK_TEST_CHUNK_BASE)
        return 0;
    size_t offset = (value - PODLINK_TEST_CHUNK_BASE) * PODLINK_TEST_CHUNK_SIZE;
    if (offset >= sizeof(snapshot))
        return 0;
    size_t count = MIN(PODLINK_TEST_CHUNK_SIZE, sizeof(snapshot) - offset);
    if (length != count)
        return 0;

    int level = disable_irq_save();
    bool ready = state == CAPTURE_READY;
    if (ready)
        memcpy(reply, (const uint8_t *)snapshot + offset, count);
    restore_irq(level);
    return ready ? count : 0;
}
