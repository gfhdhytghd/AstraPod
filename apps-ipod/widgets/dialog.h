/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to dialog.c: the dialog_style descriptor and the frame/inset
 * helpers.
 ****************************************************************************/

/* Native dialog styling and input. The box painter also serves splash and
 * other transient widgets. */
#ifndef _GUI_DIALOG_H_
#define _GUI_DIALOG_H_

#include <stdbool.h>
#include "draw/screen_access.h"
#include "draw/viewport.h"

struct button_mapping;

struct dialog;

/* ---------------------------------------------------------------------- *
 * Style                                                                  *
 * ---------------------------------------------------------------------- */

/* A colour that resolves, at draw time, to the one the screen already carries
 * (foreground for fg/border fields, background for bg fields). */
#define DIALOG_COLOR_INHERIT ((unsigned)-1)

/* A font that resolves to the one the parent (theme) viewport already carries. */
#define DIALOG_FONT_INHERIT  (-1)

struct dialog_style
{
    /* the box itself */
    unsigned box_fg;                /* interior text and graphics           */
    unsigned box_bg;                /* interior fill                        */
    unsigned box_border_color;
    int      box_border_width;      /* 0 == borderless                      */
    int      box_margin;            /* content inset inside the box         */
    int      box_font;
    int      edge_margin;           /* minimum side margins for confirmations */

    /* the button row (the yes/no and text-input dialogs share one renderer) */
    unsigned button_fg;
    unsigned button_bg;
    unsigned button_border_color;
    unsigned button_fg_selected;
    unsigned button_bg_selected;
    unsigned button_border_color_selected;
    int      button_border_width;
    int      button_border_radius;
    int      button_font;
    int      button_gap, button_pad_x, button_pad_y; /* confirmation layout */
    bool     glass;                 /* Classic dark glossy panel            */
    bool     popup;                 /* Classic 2009 navy glass modal alert  */
};

/* Whether the active native theme uses Classic / Fruit dialogs. */
bool dialog_is_classic(void);

/* Resolve the active native modal tokens (2009 navy alert under Classic). */
void dialog_style_default(struct dialog_style *s);

/* Resolve modal tokens for a brief dark glossy glass overlay under Classic;
 * identical to dialog_style_default() under Scrim. */
void dialog_style_glass(struct dialog_style *s);

/* Classic shared modal chrome helpers (no-op or fallback under Scrim). */
void dialog_draw_glass_panel(struct screen *screen, int x, int y,
                             int w, int h, int r, bool opaque);
void dialog_draw_popup_panel(struct screen *screen, int x, int y,
                             int w, int h, bool opaque);
void dialog_draw_popup_pair_footer(struct screen *screen, int x, int y,
                                   int w, int h,
                                   const char *left_label,
                                   const char *left_hint, bool left_arrow,
                                   const char *right_label,
                                   const char *right_hint, bool right_arrow,
                                   int label_font, int hint_font);
void dialog_draw_popup_single_footer(struct screen *screen, int x, int y,
                                     int w, int h, const char *label,
                                     const char *hint, bool scrollable,
                                     int label_font, int hint_font);
void dialog_draw_popup_action_row(struct screen *screen, int x, int y,
                                  int w, int h, const char *key_text,
                                  bool key_arrow_right,
                                  const char *action_text, bool primary,
                                  int key_font, int label_font);
void dialog_draw_progress(struct screen *screen, int x, int y, int w, int h,
                          int current, int total);
void dialog_draw_shadow_text(struct screen *screen, int x, int y,
                             const char *text, unsigned fg, unsigned shadow,
                             int dy);
/* Handle playback actions (PLAY/skip) inside modal dialogs. */
bool dialog_handle_playback(int action);

/* How much `style` takes out of the box on each side (the margin). Sizing code
 * (a measure() callback) uses this to convert a content size into a box size. */
struct dialog_insets { int left, top, right, bottom; };
void dialog_get_insets(const struct dialog_style *style,
                       struct dialog_insets *out);

/* ---------------------------------------------------------------------- *
 * Frame primitive                                                        *
 * ---------------------------------------------------------------------- */

/* Draw an opaque, bordered box into `box`, which MUST already be the active
 * viewport on `screen` (carrying the final screen-relative geometry). It is
 * left active. `box` is not re-set here because set_viewport() reloads fg/bg
 * from the viewport and would undo a caller's colour override, which is how the
 * popup's broken-theme fallback reaches DIALOG_COLOR_INHERIT.
 *
 * `content_out` is filled with the content sub-viewport: `box` inset per
 * dialog_get_insets(), i.e. by the style's margin on every side. It is NOT made
 * active and NOT flushed - the caller draws the interior into it and owns the
 * update_viewport()/flush.
 *
 * `box`'s own colour patterns are updated to the style's resolved colours (and
 * inherited by content_out), so re-setting either viewport active keeps the
 * style rather than reloading the theme's colours.
 */
void dialog_frame_box(struct screen *screen, struct viewport *box,
                      const struct dialog_style *style,
                      struct viewport *content_out);

/* Draw a labelled button at (x, y, w, h) in the active viewport's coordinates,
 * in the style's normal or selected colours. Shared by the yes/no and
 * text-input dialogs. The screen's drawmode, foreground and font are restored
 * afterwards. */
void dialog_draw_button(struct screen *screen,
                        const struct dialog_style *style,
                        int x, int y, int w, int h,
                        const char *label, bool selected);

/* A box's own title, at the top of its content: centred in the theme's bold
 * UI font, a rule under it. A theme's status bar need not show a title, so
 * a box carries its own. The height it takes (0 for a NULL title), and
 * drawing it into the active content viewport. */
int dialog_title_height(const char *title);
void dialog_draw_title(struct screen *screen, struct viewport *content,
                       const char *title);

/* Where the label sits. Centred is right for a Cancel/OK pair; left is right
 * for a button that is really a row of a list. */
enum dialog_button_align {
    DIALOG_BTN_CENTRE = 0,
    DIALOG_BTN_LEFT,
};

/* As above, with the three things a list row needs that a Cancel/OK pair does
 * not:
 *
 *   `align`  - see above.
 *   `icon`   - optional 1bpp bitmap drawn at the label's left, in the label's
 *              colour, with the text indented past it. NULL for none.
 *   `scroll_px` - how far the label is scrolled left, in pixels. Clamped to
 *              what actually overflows, so 0 is always "start of the text".
 *   `overflow_out` - if non-NULL, receives how many pixels the label overflows
 *              the space available to it (0 when it fits). That is the range
 *              scroll_px is clamped to, so a caller animating the text has the
 *              one number it needs.
 *
 * Scrolling is the caller's to drive, deliberately: the shared scroll engine
 * redraws asynchronously using whatever colours are current when its thread
 * runs, which on a selected row is the wrong background and text in the
 * background's colour. A dialog repaints on every pass anyway, so stepping an
 * offset there costs nothing and is the same every frame.
 *
 * The label is always clipped to the button's interior, so it can never spill
 * over the border the way a bare putsxy() does. */
void dialog_draw_button_ex(struct screen *screen,
                           const struct dialog_style *style,
                           int x, int y, int w, int h,
                           const char *label, bool selected,
                           enum dialog_button_align align,
                           const struct bitmap *icon,
                           int scroll_px, int *overflow_out);

/* A wrapped display line: a (non-terminated) slice of the source text. */
struct dialog_text_line
{
    const char *str;
    int len;
};

/* Greedy word-wrap `text` into display lines no wider than `max_width` pixels
 * in `font`, writing up to `max_lines` slices into `out` (each pointing into
 * `text`). A line break in the text ends a line, so a blank line between
 * paragraphs comes out as an empty slice; a word wider than max_width is
 * broken where it reaches the edge. Returns the number of lines written. */
int dialog_wrap_text(const char *text, int max_width, int font,
                     struct dialog_text_line *out, int max_lines);

/* ---------------------------------------------------------------------- *
 * Interactive dialog                                                     *
 * ---------------------------------------------------------------------- */

enum dialog_disposition
{
    DIALOG_CONTINUE = 0,   /* keep looping                                 */
    DIALOG_ACCEPT,         /* close: OK / yes outcome                      */
    DIALOG_CANCEL,         /* close: cancel / no outcome                   */
    DIALOG_ABORT,          /* close: system event (e.g. USB); the callback *
                            * has stashed specifics in its own `data`      */
};

struct dialog_callbacks
{
    /* Fill `box` with the desired frame geometry (screen-relative x/y/w/h) for
     * `screen`. Called before each (re)draw with the theme-enabled parent
     * viewport active, so its font is set and text metrics are valid here.
     * NULL gives draw() an unframed full-screen viewport. */
    void (*measure)(struct dialog *d, struct screen *s,
                    struct viewport *box, void *data);

    /* Draw the interior into `content` - the framed box's content viewport,
     * already active and already inset past the style's margin. The callback
     * owns its own sub-viewports (message block, button row, edit line, ...)
     * but not the outer padding. */
    void (*draw)(struct dialog *d, struct screen *s,
                 struct viewport *content, void *data);

    /* Handle one action - ACTION_NONE is delivered on each idle/poll timeout,
     * so timeout countdowns and expiry are handled here. Return a
     * dialog_disposition. The loop repaints the box on every pass (the
     * status-bar skin dirties the framebuffer inside get_action), so callbacks
     * just mutate state; the next redraw reflects it. */
    int  (*on_action)(struct dialog *d, int action, void *data);

    /* Optional (may be NULL): called once after the loop but before the theme
     * is undone, so a closing message can be drawn in the same theme scope
     * (e.g. the yes/no result message). */
    void (*on_close)(struct dialog *d, void *data);

};

struct dialog
{
    /* --- set by dialog_init(), read-only to callbacks --- */
    const struct dialog_callbacks *cb;
    void *data;
    int   context;              /* CONTEXT_* passed to get_action()         */
    const char *title;          /* SBS persistent title, or NULL for none   */
    struct dialog_style style;  /* resolved copy; measure/draw may read it  */

    /* --- maintained by dialog_run(), readable by callbacks --- */
    struct viewport parent[NB_SCREENS]; /* theme-enabled parent per screen  */
    bool  default_style;
    bool  backlight_on;         /* backlight state sampled before get_action */

    /* The get_action timeout for the NEXT pass, seeded from dialog_run()'s
     * argument. A callback may write it to change how often it is woken with
     * ACTION_NONE -- a dialog that animates something can ask for a fast tick
     * only while the animation is running, and go back to idling after. Every
     * pass costs a full repaint of the box (see dialog_run), so this is the
     * difference between animating and burning CPU. */
    int   poll_ticks;

    /* --- optional, set after dialog_init() --- */
    const char *motion_route; /* literal name in the frame-time trace */
    /* Under Classic / Fruit themes, draw as a centred dark glossy panel with
     * fade+zoom instead of the default light modal pop-up. */
    bool  glass;
    /* Under Classic / Fruit themes, take the full screen below the status bar
     * with a horizontal slide (used by long list pickers: search, folders). */
    bool  full_page;
    /* Drawn into `fb`, fb_w by fb_h, over whatever it holds, and shown by
     * present() after each repaint, instead of on the screen: for a page
     * the text viewer has turned sideways. NULL draws on the screen. */
    struct frame_buffer_t *fb;
    int   fb_w, fb_h;
    void (*present)(void);
    /* The buttons turned with it: a keymap for get_custom_action(), in
     * place of `context`. NULL reads `context`. */
    const struct button_mapping *(*keymap)(int context);
};

/* Initialise a dialog. `title` and `style` may be NULL (NULL style == the
 * active native theme); `style` is copied. The struct
 * is caller-allocated (typically on the stack); treat the fields as opaque. */
void dialog_init(struct dialog *d, int context, const char *title,
                 const struct dialog_style *style,
                 const struct dialog_callbacks *cb, void *data);

/* Present an interactive sheet. Polls its controller at poll_ticks,
 * with additional compositor deadlines while its transition is active. */
int dialog_run(struct dialog *d, int poll_ticks);
/* Restore the underlying page before a higher-priority screen is opened. */
void dialog_interrupt_transition(void);
#endif /* _GUI_DIALOG_H_ */
