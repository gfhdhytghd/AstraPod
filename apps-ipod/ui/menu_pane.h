/* PodBox split-screen menu pane. GPL version 2+. */
#ifndef PODBOX_UI_MENU_PANE_H
#define PODBOX_UI_MENU_PANE_H

#include <stdbool.h>
#include "lcd.h"
#include "retained.h"
#include "widgets/list.h"

/* Under style.menu_pane=split a menu's rows and status bar take the left
 * half and the pane the right half, full height. Its shadow lies over the
 * last columns of the rows. */
#define MENU_PANE_X (LCD_WIDTH / 2)

struct stock_canvas;

struct menu_pane_content {
    enum list_pane_kind kind;
    int art;                    /* enum stock_art_id, or -1 */
    char title[96];
    char caption[96];
    /* LIST_PANE_PICTURE: the art's top-left on screen and the title's
     * baseline; the caption's lies 21 rows lower. */
    int x, y, baseline;
    /* LIST_PANE_PICTURE: drawn over the backdrop before the art, or NULL. */
    void (*paint)(struct stock_canvas *band);
    int stamp;                  /* a new value repaints the pane */
};

/* Whether the current theme splits menus. */
bool menu_pane_themed(void);
#if defined(HAVE_USBSTACK)
void menu_pane_status(char *out, size_t size);
#endif
/* Show `content` for a list. A new `identity` (a list generation) shows it
 * at once; a new row of the same list changes the pane after a pause. */
void menu_pane_select(const struct menu_pane_content *content,
                      unsigned identity);
/* What a LIST_PANE_PICTURE row shows: `art` at (x, y), then `title` and
 * `caption` centred in the title font. A list's pane callback sets it for
 * the row it is asked about before returning LIST_PANE_PICTURE. */
void menu_pane_set_picture(const struct menu_pane_content *content);
/* The pane and its shadow in screen coordinates. */
struct ui_rect menu_pane_rect(void);
/* Draw the part of the pane in `clip` to the default framebuffer. */
void menu_pane_paint(struct ui_rect clip);
/* Advance the pane's motion by drawing only what moved, and return the
 * rectangle drawn, empty when nothing was. */
struct ui_rect menu_pane_step(void);
/* Kernel ticks until the pane next moves; TIMEOUT_BLOCK when it is still. */
int menu_pane_deadline(void);

/* The list renderer reports each page it paints, split or not. */
void menu_pane_page(bool split);
/* The pane's left edge while a split page is on screen, else 0. The status
 * bar and page transitions follow it. */
int menu_pane_split(void);
/* A screen that owned the chrome at `depth` has left it. */
void menu_pane_viewport_left(int depth);
/* Whether the framebuffer shows a split page, for a transition taking it
 * as its outgoing page. The next page painted answers the next call. */
bool menu_pane_take_screen(void);
/* A retained page has been copied back to the framebuffer. */
void menu_pane_restore_screen(bool split);
/* Retain the last slideshow pose during Cover Flow. Pin returns the entire
 * texture with its screen-space bounds, or NULL for a still preview or a
 * blend. The caller must put the returned pointer after compositing. */
void menu_pane_hold_cover(void);
const fb_data *menu_pane_pin_cover(struct ui_rect *area, int *stride);
void menu_pane_resume_cover(bool restored);
/* The whole of the pane's backdrop in a screen-sized buffer, the first
 * frame of Now Playing entered from a split menu. */
void menu_pane_backdrop(fb_data *buffer);
/* Cast the rows' shadow on columns `edge` onwards of row `y` of a
 * framebuffer, stopping short of `limit`. */
void menu_pane_shadow(fb_data *row, int y, int edge, int limit);

#endif
