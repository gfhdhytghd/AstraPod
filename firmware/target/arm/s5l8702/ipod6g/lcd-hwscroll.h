/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_HWSCROLL_H
#define LCD_HWSCROLL_H

#include <stdbool.h>
#include <stdint.h>

/* Panel vertical scrolling (DCS 0x33/0x37) on LCD type 1.
 *
 * The scroll area runs from `top` to the bottom of the panel; the rows above
 * it stay fixed. While the start line is moved, the panel's memory is a ring
 * over the area: every transfer the driver queues is addressed through it, so
 * the framebuffer in RAM stays in screen coordinates and any screen drawn by
 * lcd_update_rect() lands where it is drawn. Transfers that bypass the queue
 * (interrupts off, no TE) first return the panel to normal mode. */

#define LCD_HWSCROLL_BANDS 8

/* Screen coordinates of rows to send from the framebuffer. */
struct lcd_hwscroll_band {
    int16_t x, y, width, height;
};

struct lcd_hwscroll_frame {
    int16_t top;     /* first row of the scroll area */
    int16_t shift;   /* rows the area's content moves up; negative moves down */
    uint8_t count;   /* bands in use */
    struct lcd_hwscroll_band band[LCD_HWSCROLL_BANDS];
};

/* Thread context. Queues one frame: in the panel's blanking gap before one
 * refresh, moves the start line by `shift` and sends the bands, so that the
 * refresh shows the whole frame. Bands must cover every pixel of the area
 * that does not equal the last frame's pixel `shift` rows further down.
 * False when the frame cannot be shown this way (TE not tracked, the area
 * differs from the one in use while it is scrolled, too many rows for the
 * gap); nothing was sent, and the caller submits the frame by
 * lcd_update_rect(). */
bool lcd_hwscroll_present(const struct lcd_hwscroll_frame *frame);

/* The TE rise of the refresh that first shows the last frame
 * lcd_hwscroll_present() accepted. */
bool lcd_hwscroll_presented(uint32_t *rise);

/* The start line's distance from `top`, 0 in normal mode. */
int lcd_hwscroll_offset(void);

/* Transfers queued so far that reach below the top of the area the last
 * lcd_hwscroll_present() named, its own frames included. A frame is only
 * right on top of what the caller itself queued last: read this after
 * queuing, and present a frame only while it has not moved. */
unsigned int lcd_hwscroll_writes(void);

/* Driver hooks. */
int lcd_hwscroll_map(int y);            /* screen row -> panel memory row */
int lcd_hwscroll_run(int y, int rows);  /* rows from y before the ring wraps */
bool lcd_hwscroll_reset(void);          /* bus held, panel awake: normal mode */
void lcd_hwscroll_forget(void);         /* the panel lost its state */
void lcd_hwscroll_queued(int y, int height);  /* lcd_te_submit() */
bool lcd_hwscroll_pending(void);
void lcd_hwscroll_cancel(void);         /* IRQs masked */
void lcd_hwscroll_timer(uint32_t now);  /* INT_TIMERH while pending */
bool lcd_hwscroll_dma_done(void);       /* true when a frame owned the DMA */

/* Provided by the LCD driver. lcd_hwscroll_stage() locks the LCD and
 * returns a free stage buffer once no transfer is pending, or NULL;
 * lcd_hwscroll_staged() unlocks it, after mirroring rows y..y+height-1 of
 * the framebuffer and waiting for the panel unless async, when `queued`. */
uint16_t *lcd_hwscroll_stage(void);
void lcd_hwscroll_staged(int y, int height, bool queued);
/* IRQ context, bus drained or draining: false if it did not start. */
bool lcd_hwscroll_start_dma(const uint16_t *pixels, int x, int y,
                            int width, int rows);

/* Probe: a zeroed request leaves scroll mode and redraws the framebuffer. */
struct lcd_hwscroll_request {
    uint16_t lines, top, height, start;
};

bool lcd_hwscroll_valid(const struct lcd_hwscroll_request *request);
int lcd_hwscroll_test(const struct lcd_hwscroll_request *request);

/* Caller must own the drained LCD bus. False if the bus did not drain
 * before `until`; nothing was sent unless it drained. */
bool lcd_hwscroll_send(uint8_t cmd, const uint8_t *data, unsigned int size);
bool lcd_hwscroll_send_until(uint8_t cmd, const uint8_t *data,
                             unsigned int size, uint32_t until);

#endif
