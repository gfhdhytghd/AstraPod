/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The iPod Classic panel's hardware scroll, emulated for ui/hwscroll.c.
 *
 * The SDL window's surface is the panel: what it holds is what the last
 * transfers left there. A frame moves the area's rows by its shift around
 * the ring the panel's memory forms, lays its bands over them from the
 * framebuffer, and compares the result with the framebuffer. A pixel that
 * differs is one the real panel would show wrong; each such frame prints a
 * line. The window then shows the framebuffer. With PODBOX_HWSCROLL_LOG
 * set, every frame prints a line, for the tests to count.
 ****************************************************************************/
#include "config.h"

#ifdef SIMULATOR

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "lcd.h"
#include "system.h"
#include "ui/hwscroll.h"

bool hwscroll_panel_present(const struct ui_hwscroll_frame *frame);
bool hwscroll_panel_presented(uint32_t *rise);
unsigned int hwscroll_panel_writes(void);
int hwscroll_panel_offset(void);

extern SDL_Surface *lcd_surface;

static fb_data seen[LCD_HEIGHT][LCD_WIDTH];
static fb_data panel[LCD_HEIGHT][LCD_WIDTH];
static unsigned int writes, frames;
static int area_top, offset;

static const fb_data *surface_row(int y)
{
    return (const fb_data *)((const char *)lcd_surface->pixels +
                             y * lcd_surface->pitch);
}

static bool surface_usable(void)
{
    return lcd_surface && lcd_surface->format->BytesPerPixel == sizeof(fb_data) &&
           lcd_surface->w >= LCD_WIDTH && lcd_surface->h >= LCD_HEIGHT &&
           !SDL_MUSTLOCK(lcd_surface);
}

/* A transfer reached the area when the surface there changed. */
unsigned int hwscroll_panel_writes(void)
{
    if (!surface_usable())
        return writes;
    bool changed = false;
    for (int y = area_top; y < LCD_HEIGHT; ++y)
    {
        const fb_data *row = surface_row(y);
        if (memcmp(seen[y], row, sizeof(seen[y])))
        {
            memcpy(seen[y], row, sizeof(seen[y]));
            changed = true;
        }
    }
    return writes += changed;
}

bool hwscroll_panel_present(const struct ui_hwscroll_frame *frame)
{
    int top = frame->top, height = LCD_HEIGHT - frame->top;
    if (!surface_usable() || top < 0 || height <= 0 ||
        (offset && top != area_top) ||
        frame->shift >= height || -frame->shift >= height)
        return false;

    for (int y = top; y < LCD_HEIGHT; ++y)
    {
        int from = top + ((y - top + frame->shift) % height + height) % height;
        memcpy(panel[y], surface_row(from), sizeof(panel[y]));
    }
    for (int i = 0; i < frame->count; ++i)
    {
        const struct ui_rect *band = &frame->band[i];
        int left = MAX(band->x, 0), right = MIN(band->x + band->width, LCD_WIDTH);
        int first = MAX(band->y, top);
        int last = MIN(band->y + band->height, LCD_HEIGHT);
        for (int y = first; y < last && left < right; ++y)
            memcpy(&panel[y][left], FBADDR(left, y),
                   (right - left) * sizeof(fb_data));
    }
    unsigned int wrong = 0;
    for (int y = top; y < LCD_HEIGHT; ++y)
        for (int x = 0; x < LCD_WIDTH; ++x)
            wrong += panel[y][x] != *FBADDR(x, y);

    ++frames;
    if (wrong || getenv("PODBOX_HWSCROLL_LOG"))
    {
        printf("hwscroll: frame %u top %d shift %d bands %d wrong %u\n",
               frames, top, frame->shift, frame->count, wrong);
        fflush(stdout);
    }
    area_top = top;
    offset = ((offset + frame->shift) % height + height) % height;
    lcd_update_rect(0, top, LCD_WIDTH, height);
    return true;
}

bool hwscroll_panel_presented(uint32_t *rise)
{
    (void)rise;
    return false;
}

int hwscroll_panel_offset(void)
{
    return offset;
}

#endif /* SIMULATOR */
