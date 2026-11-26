/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include "kernel.h"
#include "lcd.h"
#include "lcd-experiment-s5l8702.h"
#include "lcd-hwscroll.h"
#include "lcd-te-s5l8702.h"

enum { DCS_NORMAL = 0x13, DCS_SCROLL_AREA = 0x33, DCS_SCROLL_START = 0x37 };

bool lcd_hwscroll_valid(const struct lcd_hwscroll_request *request)
{
    if (!request->lines)
        return !request->top && !request->height && !request->start;

    unsigned int end = request->top + request->height;
    return (request->lines == LCD_HEIGHT || request->lines == LCD_WIDTH) &&
           request->height && end <= request->lines &&
           request->start >= request->top && request->start < end;
}

static unsigned int fixed_top, fixed_end;

static void draw_grid(uint16_t *pixels, int height, unsigned int frame)
{
    (void)frame;
    for (int y = 0; y < height; y++)
    {
        bool fixed = (unsigned int)y < fixed_top || (unsigned int)y >= fixed_end;
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            /* The white lines and color steps identify both axes; the
             * rows meant to stay fixed are solid red. */
            *pixels++ = fixed ? LCD_RGBPACK(220, 0, 0) :
                x % 40 < 2 || y % 20 < 2 ? LCD_WHITE :
                LCD_RGBPACK(32 + (x / 40) * 28, 24 + (y / 20) * 19, 72);
        }
    }
}

static bool set_scroll(const struct lcd_hwscroll_request *request)
{
    unsigned int bottom = request->lines - request->top - request->height;
    uint8_t area[] = {
        request->top >> 8, request->top & 0xff,
        request->height >> 8, request->height & 0xff,
        bottom >> 8, bottom & 0xff,
    };
    uint8_t start[] = { request->start >> 8, request->start & 0xff };
    return lcd_hwscroll_send(DCS_SCROLL_AREA, area, sizeof(area)) &&
           lcd_hwscroll_send(DCS_SCROLL_START, start, sizeof(start));
}

int lcd_hwscroll_test(const struct lcd_hwscroll_request *request)
{
    if (!lcd_hwscroll_valid(request))
        return EINVAL;
    if (!lcd_te_supported())
        return ENOTSUP;
    if (!lcd_te_acquire_bus())
        return EBUSY;

    bool ok = lcd_hwscroll_send(DCS_NORMAL, NULL, 0);
    if (ok && request->lines)
    {
        fixed_top = request->top;
        fixed_end = request->top + request->height;
        uint32_t dma = lcd_experiment_dma_total();
        lcd_experiment_present(0, LCD_HEIGHT, draw_grid, 0);
        ok = lcd_experiment_dma_total() != dma;
        if (ok)
        {
            sleep(2 * HZ);
            /* Send only commands between the baseline and shifted image. */
            struct lcd_hwscroll_request step = *request;
            ok = set_scroll(&step);
            /* Step the start line every tick for eight seconds. */
            for (int i = 1; ok && i < 8 * HZ; i++)
            {
                sleep(1);
                step.start = request->top +
                    (request->start - request->top + i) % request->height;
                uint8_t start[] = { step.start >> 8, step.start & 0xff };
                ok = lcd_hwscroll_send(DCS_SCROLL_START, start, sizeof(start));
            }
        }
        if (!lcd_hwscroll_send(DCS_NORMAL, NULL, 0))
            ok = false;
    }
    lcd_te_release_bus();
    lcd_update();
    lcd_wait_idle();
    return ok ? 0 : EIO;
}
