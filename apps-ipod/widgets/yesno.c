/***************************************************************************
 * Original code from RockBox
 * was: apps/gui/yesno.c
 * Copyright (C) 2005 by Kevin Ferrare
 * GNU General Public License (version 2+)
 *
 * Yes/no confirmation dialog with optional timeout and voiced prompts.
 * Returns YESNO_YES, _NO, _TMO or _USB.
 ****************************************************************************/
#include "config.h"
#include <stdio.h>
#include "yesno.h"
#include "system.h"
#include "kernel.h"
#include "system/shutdown.h"
#include "lang.h"
#include "input/action.h"
#include "speech/talk.h"
#include "settings/settings.h"
#include "draw/viewport.h"
#include "font.h"
#include "ui/native_theme_runtime.h"
#include "splash.h"
#include "dialog.h"

/* Message box geometry: at most the display width minus a horizontal margin,
 * height fitted to the content and centred vertically. The inner padding
 * between the border and the contents is the style's box_margin. */
#define YN_MAX_LINES 16 /* cap on wrapped message lines */

/* Per-run yes/no state, handed to the dialog callbacks via `data`. */
struct yesno_ctx
{
    const struct text_message *message;
    enum yesno_res selection;      /* highlighted button */
    enum yesno_res tmo_default;    /* result on timeout, or YESNO_TMO for none */
    int  tmo_secs;                 /* initial timeout in seconds (button sizing) */
    long end_tick;
    bool talked;                   /* the question has been said */
    bool talk_menu;
    const struct text_message *yes_message; /* shown on accept, optional */
    const struct text_message *no_message;  /* shown on cancel, optional */
    enum yesno_res result;
    bool defer_usb;                /* return YESNO_USB without handling it */
    int yes_id, no_id;             /* the buttons' labels, as lang ids */
    bool back_cancels;             /* Back is YESNO_CANCEL, not YESNO_NO */
    /* message word-wrapped to the box width, rebuilt by yesno_measure() */
    struct dialog_text_line wrap[YN_MAX_LINES];
    int wrap_count;
    /* Classic 2.0.5 modal pop-up layout: bold heading (line 0) and fonts */
    struct dialog_text_line head_wrap[6];
    int head_count;
    int head_font;
    int btn_font;
};

static void talk_text_message(const struct text_message * message, bool enqueue)
{
    int line;
    for(line=0; line < message->nb_lines; line++)
    {
        const char *text = message->message_lines[line];
        long id = P2ID((unsigned char *)text);
        if(id>=0)
            talk_id(id, enqueue);
        else if (text && *text)
            talk_text(text, enqueue);   /* only the synthesiser can */
        else
            continue;
        enqueue = true;
    }
}

static void talk_button(const struct yesno_ctx *c,
                        enum yesno_res selection, bool enqueue)
{
    talk_id(selection == YESNO_YES ? c->yes_id : c->no_id, enqueue);
}

/* Seconds left on the timeout, or -1 when there is no timeout default. */
static int yesno_tmo_remaining(struct yesno_ctx *c)
{
    if (c->tmo_default != YESNO_YES && c->tmo_default != YESNO_NO)
        return -1;
    int r = (c->end_tick - current_tick) / HZ;
    return r < 0 ? 0 : r;
}

/* Build the two button labels; the timeout-default button gets " (secs)"
 * appended (secs < 0 appends nothing). */
static void yesno_button_labels(struct yesno_ctx *c, int secs,
                                char *yes, char *no, size_t sz)
{
    if (secs >= 0 && c->tmo_default == YESNO_YES)
        snprintf(yes, sz, "%s (%d)", str(c->yes_id), secs);
    else
        snprintf(yes, sz, "%s", str(c->yes_id));

    if (secs >= 0 && c->tmo_default == YESNO_NO)
        snprintf(no, sz, "%s (%d)", str(c->no_id), secs);
    else
        snprintf(no, sz, "%s", str(c->no_id));
}

/* Widest button width for this dialog: sized to the initial countdown so the
 * button (and box) don't jitter as the seconds tick down. */
static int yesno_button_width(struct yesno_ctx *c, struct screen *display,
                               const struct dialog_style *style)
{
    char yes[24], no[24];
    int w_yes, w_no;
    yesno_button_labels(c, c->tmo_secs, yes, no, sizeof yes);
    display->getstringsize(yes, &w_yes, NULL);
    display->getstringsize(no,  &w_no,  NULL);
    return MAX(w_yes, w_no) + style->button_pad_x * 2;
}

static void yesno_draw_buttons(struct yesno_ctx *c, struct dialog *d,
                               struct screen *display, struct viewport *content)
{
    char yes[24], no[24];
    int height;
    const struct dialog_style *style = &d->style;
    yesno_button_labels(c, yesno_tmo_remaining(c), yes, no, sizeof(yes));
    display->getstringsize(yes, NULL, &height);
    int gap = MIN(style->button_gap, content->width);
    int width = MIN(yesno_button_width(c, display, style), (content->width - gap) / 2);
    if (width <= 0)
        return;

    height += style->button_pad_y * 2;
    int x = (content->width - (width * 2 + gap)) / 2;
    int y = MAX(0, content->height - height);
    dialog_draw_button(display, style, x, y, width, height, yes, c->selection == YESNO_YES);
    dialog_draw_button(display, style, x + width + gap, y, width, height,
                       no, c->selection == YESNO_NO);
}

/* --- dialog callbacks --- */

/* Word-wrap the message lines to the box content width into c->wrap. */
static void yesno_wrap_message(struct yesno_ctx *c, int content_w, int font)
{
    c->wrap_count = 0;
    for (int i = 0; i < c->message->nb_lines &&
                    c->wrap_count < YN_MAX_LINES; i++)
    {
        const char *line = P2STR((unsigned char *)c->message->message_lines[i]);
        int n = dialog_wrap_text(line, content_w, font,
                                 &c->wrap[c->wrap_count],
                                 YN_MAX_LINES - c->wrap_count);
        if (n == 0)                 /* preserve a blank message line */
        {
            c->wrap[c->wrap_count].str = "";
            c->wrap[c->wrap_count].len = 0;
            n = 1;
        }
        c->wrap_count += n;
    }
}

static void yesno_measure(struct dialog *d, struct screen *s,
                          struct viewport *box, void *data)
{
    struct yesno_ctx *c = data;
    struct dialog_insets in;
    int area_h = s->lcdheight;
    int sw = s->lcdwidth;

    if (dialog_is_classic())
    {
        const int pad_x = 16;
        const int pad_top = 14;
        const int pad_mid = 14;
        int box_w = MIN(sw - 40, 272);
        int text_w = box_w - 2 * pad_x;
        int hfont = c->head_font >= 0 ? c->head_font : box->font;
        int hch = font_get(hfont)->height;
        int bch = font_get(box->font)->height;
        int foot_h = 28 + 4 + bch + 8;

        d->style.box_margin = 0;
        c->head_count = 0;
        c->wrap_count = 0;
        int body_start = 0;
        if (d->title)
        {
            c->head_count = dialog_wrap_text(d->title, text_w, hfont,
                                             c->head_wrap,
                                             ARRAYLEN(c->head_wrap));
        }
        else if (c->message->nb_lines > 0)
        {
            const char *line = P2STR((unsigned char *)c->message->message_lines[0]);
            c->head_count = dialog_wrap_text(line, text_w, hfont,
                                             c->head_wrap,
                                             ARRAYLEN(c->head_wrap));
            body_start = 1;
        }
        for (int i = body_start; i < c->message->nb_lines &&
                                 c->wrap_count < YN_MAX_LINES; i++)
        {
            const char *line = P2STR((unsigned char *)c->message->message_lines[i]);
            int n = dialog_wrap_text(line, text_w, box->font,
                                     &c->wrap[c->wrap_count],
                                     YN_MAX_LINES - c->wrap_count);
            if (n == 0)
            {
                c->wrap[c->wrap_count].str = "";
                c->wrap[c->wrap_count].len = 0;
                n = 1;
            }
            c->wrap_count += n;
        }

        int text_h = 0;
        if (c->head_count > 0)
            text_h += c->head_count * (hch + 2) - 2;
        if (c->wrap_count > 0)
        {
            if (text_h > 0)
                text_h += 6;
            text_h += c->wrap_count * (bch + 2) - 2;
        }
        int box_h = pad_top + text_h + pad_mid + foot_h;
        box_h = MIN(area_h - 24, MAX(120, box_h));

        box->width = box_w;
        box->height = box_h;
        box->x = (sw - box_w) / 2;
        box->y = (area_h - box_h) / 2;
        return;
    }

    /* Size and centre against the physical display, not the theme content
     * viewport the box arrives as. box->x/y below are absolute screen
     * coordinates, so measuring against s->getwidth()/getheight() (the theme
     * viewport, which a themed SBS can inset - e.g. %Vi(-,3,28,302,-,-)) would
     * centre the box on the viewport's size but position it from absolute 0,
     * leaving it visibly off-centre on the display. */
    int ch = s->getcharheight();

    dialog_get_insets(&d->style, &in);

    /* wrap to the widest the content may grow to, then size the box to the
     * actual content (widest wrapped line or the button row) and centre it. */
    int box_max_w = sw - 2 * d->style.edge_margin;
    int max_content_w = box_max_w - in.left - in.right;
    yesno_wrap_message(c, max_content_w, box->font);

    int text_w = 0;
    for (int i = 0; i < c->wrap_count; i++)
    {
        int w = font_getstringnsize((const unsigned char *)c->wrap[i].str,
                                    c->wrap[i].len, NULL, NULL, box->font);
        if (w > text_w)
            text_w = w;
    }

    int btn_row_w = yesno_button_width(c, s, &d->style) * 2 + d->style.button_gap;

    int box_w = MAX(text_w, btn_row_w) + in.left + in.right;
    if (box_w > box_max_w)
        box_w = box_max_w;         /* clamp to the display */

    int btn_h = ch + 2 * d->style.button_pad_y;      /* button height (matches buttons) */

    /* Gap between the message and the buttons. The last text line reserves
     * descender space under its baseline that is usually empty, so subtract
     * that from the margin -- otherwise the visible gap here is the margin
     * plus a descender, noticeably wider than the margins above and below. */
    int gap = in.top - (ch - font_get(box->font)->ascent);
    if (gap < 0)
        gap = 0;
    int nlines = c->wrap_count > 0 ? c->wrap_count : 1;
    int content_h = nlines * ch + gap + btn_h;
    int box_h = content_h + in.top + in.bottom;
    if (box_h > area_h)
        box_h = area_h;                     /* clamp to the display */

    box->x = (sw - box_w) / 2;
    box->width = box_w;
    box->y = (area_h - box_h) / 2;
    box->height = box_h;
}

static void yesno_draw(struct dialog *d, struct screen *s,
                       struct viewport *content, void *data)
{
    struct yesno_ctx *c = data;
    if (dialog_is_classic())
    {
        const int pad_top = 14;
        int bfont = c->btn_font >= 0 ? c->btn_font : content->font;
        int hfont = c->head_font >= 0 ? c->head_font : content->font;
        int kfont = content->font;
        int hch = font_get(hfont)->height;
        int bch = font_get(content->font)->height;
        int foot_h = 28 + 4 + bch + 8;
        int foot_y = content->height - foot_h;
        unsigned shadow = LCD_RGBPACK(4, 10, 24);

        int text_h = 0;
        if (c->head_count > 0)
            text_h += c->head_count * (hch + 2) - 2;
        if (c->wrap_count > 0)
        {
            if (text_h > 0)
                text_h += 6;
            text_h += c->wrap_count * (bch + 2) - 2;
        }
        int avail = foot_y - pad_top - 10;
        int y = pad_top + (avail > text_h ? (avail - text_h) / 2 : 0);

        s->set_drawmode(DRMODE_FG);
        if (c->head_count > 0)
        {
            int old_font = content->font;
            s->setfont(hfont);
            for (int i = 0; i < c->head_count && y + hch <= foot_y - 6; i++)
            {
                int lw = font_getstringnsize(
                    (const unsigned char *)c->head_wrap[i].str,
                    c->head_wrap[i].len, NULL, NULL, hfont);
                int tx = MAX(12, (content->width - lw) / 2);
                s->set_foreground(shadow);
                s->putsxyf(tx, y - 1, "%.*s",
                           c->head_wrap[i].len, c->head_wrap[i].str);
                s->set_foreground(LCD_RGBPACK(255, 255, 255));
                s->putsxyf(tx, y, "%.*s",
                           c->head_wrap[i].len, c->head_wrap[i].str);
                y += hch + 2;
            }
            s->setfont(old_font);
            y += 4;
        }
        for (int i = 0; i < c->wrap_count && y + bch <= foot_y - 6; i++)
        {
            int lw = font_getstringnsize(
                (const unsigned char *)c->wrap[i].str,
                c->wrap[i].len, NULL, NULL, content->font);
            int tx = MAX(12, (content->width - lw) / 2);
            s->set_foreground(shadow);
            s->putsxyf(tx, y - 1, "%.*s", c->wrap[i].len, c->wrap[i].str);
            s->set_foreground(LCD_RGBPACK(234, 240, 250));
            s->putsxyf(tx, y, "%.*s", c->wrap[i].len, c->wrap[i].str);
            y += bch + 2;
        }

        char yes[24], no[24];
        yesno_button_labels(c, yesno_tmo_remaining(c), yes, no, sizeof(yes));
        dialog_draw_popup_pair_footer(s, 0, foot_y, content->width, foot_h,
                                      no, c->back_cancels ? "LEFT" : "/ MENU",
                                      true, yes, "SELECT /", true,
                                      bfont, kfont);
        s->set_drawmode(DRMODE_SOLID);
        return;
    }

    int ch = s->getcharheight();

    /* message: centred, word-wrapped, in the area above the buttons */
    struct viewport txt = *content;
    txt.height = (c->wrap_count > 0 ? c->wrap_count : 1) * ch;
    txt.flags |= VP_FLAG_ALIGN_CENTER;
    s->set_viewport(&txt);
    /* DRMODE_FG so glyphs draw over our solid fill without the theme backdrop
     * bleeding into each character's background */
    s->set_drawmode(DRMODE_FG);
    for (int i = 0; i < c->wrap_count; i++)
        s->putsxyf(0, i * ch, "%.*s", c->wrap[i].len, c->wrap[i].str);

    s->set_viewport(content);
    yesno_draw_buttons(c, d, s, content);
}

static const struct button_mapping *yesno_classic_keymap(int context)
{
#if BUTTON_REMOTE != 0
    if (context & CONTEXT_REMOTE)
        return get_context_mapping(CONTEXT_YESNOSCREEN | CONTEXT_REMOTE);
#endif
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_YESNO_ACCEPT, BUTTON_SELECT | BUTTON_REL,    BUTTON_SELECT },
        { ACTION_YESNO_ACCEPT, BUTTON_RIGHT | BUTTON_REL,     BUTTON_RIGHT  },
        { ACTION_STD_CANCEL,   BUTTON_LEFT | BUTTON_REL,      BUTTON_LEFT   },
        { ACTION_STD_MENU,     BUTTON_MENU | BUTTON_REL,      BUTTON_MENU   },
        { ACTION_WPS_PLAY,     BUTTON_PLAY | BUTTON_REL,      BUTTON_PLAY   },
        { ACTION_NONE,         BUTTON_SELECT | BUTTON_REPEAT, BUTTON_NONE   },
        { ACTION_NONE,         BUTTON_RIGHT | BUTTON_REPEAT,  BUTTON_NONE   },
        { ACTION_NONE,         BUTTON_LEFT | BUTTON_REPEAT,   BUTTON_NONE   },
        { ACTION_NONE,         BUTTON_MENU | BUTTON_REPEAT,   BUTTON_NONE   },
        { ACTION_NONE,         BUTTON_PLAY | BUTTON_REPEAT,   BUTTON_NONE   },
        LAST_ITEM_IN_LIST
    };
    return keys;
}

static int yesno_on_action(struct dialog *d, int action, void *data)
{
    struct yesno_ctx *c = data;

    /* The question once, then the button it starts on */
    if (c->talk_menu && !c->talked)
    {
        c->talked = true;
        talk_text_message(c->message, false);
        talk_button(c, c->selection, true);
    }

    if (dialog_is_classic())
    {
        switch (action)
        {
            case ACTION_YESNO_ACCEPT:
                if (!d->backlight_on)
                    return DIALOG_CONTINUE;
                c->result = YESNO_YES;
                return DIALOG_ACCEPT;
            case ACTION_STD_CANCEL:
                if (!d->backlight_on)
                    return DIALOG_CONTINUE;
                c->result = YESNO_NO;
                return DIALOG_CANCEL;
            case ACTION_STD_MENU:
                if (!d->backlight_on)
                    return DIALOG_CONTINUE;
                c->result = c->back_cancels ? YESNO_CANCEL : YESNO_NO;
                return DIALOG_CANCEL;
            case ACTION_WPS_PLAY:
                dialog_handle_playback(action);
                return DIALOG_CONTINUE;
            default:
                break;
        }
    }

    switch (action)
    {
        case ACTION_YESNO_ACCEPT:
            if (!d->backlight_on)
                break;              /* don't accept while the screen is off */
            c->result = c->selection; /* confirm the highlighted button */
            return (c->selection == YESNO_YES) ? DIALOG_ACCEPT : DIALOG_CANCEL;
        case ACTION_STD_PREV: /* scroll back / left -> Yes (left button) */
        case ACTION_STD_NEXT: /* scroll fwd / right -> No (right button) */
        {
            enum yesno_res sel = action == ACTION_STD_PREV ? YESNO_YES
                                                           : YESNO_NO;
            if (sel != c->selection && c->talk_menu)
                talk_button(c, sel, false);
            c->selection = sel;
            break;
        }
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            if (!d->backlight_on)
                break;
            c->result = c->back_cancels ? YESNO_CANCEL : YESNO_NO;
            return DIALOG_CANCEL;
        case ACTION_NONE:
            if (c->tmo_default != YESNO_TMO &&
                TIME_AFTER(current_tick, c->end_tick))
            {
                splash(HZ/2, ID2P(LANG_TIMEOUT));
                c->result = c->tmo_default;
                return DIALOG_ACCEPT;
            }
            break;
        case ACTION_UNKNOWN:
        case ACTION_REDRAW: /* handled by the per-pass repaint */
            break;
        default:
            /* deferred, USB is reported without being handled */
            if ((c->defer_usb && action == SYS_USB_CONNECTED) ||
                default_event_handler(action) == SYS_USB_CONNECTED)
            {
                c->result = YESNO_USB;
                return DIALOG_ABORT;
            }
            /* ignore unmapped buttons; the choice is explicit via the
             * highlighted Yes/No buttons */
            break;
    }
    return DIALOG_CONTINUE;
}

static void yesno_on_close(struct dialog *d, void *data)
{
    (void)d;
    struct yesno_ctx *c = data;
    const struct text_message *resmsg = NULL;

    if (c->result == YESNO_YES)
        resmsg = c->yes_message;
    else if (c->result == YESNO_NO)
        resmsg = c->no_message;

    if (resmsg == NULL)
        return;

    if (c->talk_menu)
    {
        talk_text_message(resmsg, false);
        talk_force_enqueue_next();
    }

    char message[MAX_PATH];
    size_t used = 0;
    for (int i = 0; i < resmsg->nb_lines && used < sizeof(message) - 1; i++)
    {
        const char *line = P2STR((const unsigned char *)resmsg->message_lines[i]);
        if (!line)
            continue;
        size_t room = sizeof(message) - used;
        int written = snprintf(message + used, room, "%s%s", used ? "\n" : "", line);
        if (written < 0)
            break;
        used += MIN((size_t)written, room - 1);
    }
    if (used)
        splashf(HZ, "%s", message);
}

/* Display a YES_NO prompt to the user
 *
 * ticks < HZ will be ignored and the prompt will be blocking
 * tmo_default_res is the answer that is returned when the timeout expires
 * a default result of YESNO_TMO will also make the prompt blocking
 * if tmo_default_res is YESNO_YES or YESNO_NO a seconds countdown will
 * be present next to the default option
 *
 *        ticks - timeout if (>=HZ) otherwise ignored
 *  default_res - result returned on timeout YESNO_TMO creates a blocking prompt
 * main_message - prompt to the user
 *  yes_message - displayed when YESNO_YES is choosen
 *   no_message - displayed when YESNO_NO is choosen
*/
static enum yesno_res yesno_run(int ticks, enum yesno_res tmo_default_res,
                                bool defer_usb, int yes_id, int no_id,
                                const char * title,
                                const struct text_message * main_message,
                                const struct text_message * yes_message,
                                const struct text_message * no_message)
{
    static const struct dialog_callbacks cb = {
        .measure   = yesno_measure,
        .draw      = yesno_draw,
        .on_action = yesno_on_action,
        .on_close  = yesno_on_close,
    };
    struct yesno_ctx c;
    struct dialog d;

    if (ticks < HZ) /* Display a prompt with NO timeout to the user */
        tmo_default_res = YESNO_TMO;

    c.message      = main_message;
    c.selection    = YESNO_YES;     /* default highlighted button */
    c.tmo_default  = tmo_default_res;
    c.tmo_secs     = ticks / HZ;
    c.end_tick     = current_tick + ticks;
    c.talked       = false;
    c.talk_menu    = global_settings.talk_menu;
    c.yes_message  = yes_message;
    c.no_message   = no_message;
    c.result       = YESNO_NO;
    c.defer_usb    = defer_usb;
    c.yes_id       = yes_id;
    c.no_id        = no_id;
    c.back_cancels = no_id != LANG_SET_BOOL_NO;
    c.head_font    = -1;
    c.btn_font     = -1;
    if (dialog_is_classic())
    {
        c.head_font = ui_theme_current_font_load(UI_FONT_TITLE, 64);
        c.btn_font  = ui_theme_current_font_load(UI_FONT_LIST_VALUE, 32);
    }

    dialog_init(&d, CONTEXT_YESNOSCREEN, title, NULL, &cb, &c);
    d.motion_route = "yesno";
    if (dialog_is_classic())
        d.keymap = yesno_classic_keymap;
    dialog_run(&d, HZ / 2); /* poll for statusbar and the timeout countdown */
    font_unload(c.head_font);
    font_unload(c.btn_font);
    return c.result;
}

enum yesno_res gui_syncyesno_run_w_tmo(int ticks, enum yesno_res tmo_default_res,
                                       const char * title,
                                       const struct text_message * main_message,
                                       const struct text_message * yes_message,
                                       const struct text_message * no_message)
{
    return yesno_run(ticks, tmo_default_res, false,
                     LANG_SET_BOOL_YES, LANG_SET_BOOL_NO, title,
                     main_message, yes_message, no_message);
}

enum yesno_res gui_syncyesno_run_defer_usb(
                                 const struct text_message * main_message)
{
    return yesno_run(TIMEOUT_BLOCK, YESNO_TMO, true,
                     LANG_SET_BOOL_YES, LANG_SET_BOOL_NO, NULL,
                     main_message, NULL, NULL);
}

enum yesno_res gui_syncyesno_run_labels(
                                 const struct text_message * main_message,
                                 int yes_id, int no_id)
{
    return yesno_run(TIMEOUT_BLOCK, YESNO_TMO, false, yes_id, no_id, NULL,
                     main_message, NULL, NULL);
}

enum yesno_res gui_syncyesno_run(const struct text_message * main_message,
                                 const struct text_message * yes_message,
                                 const struct text_message * no_message)
{
    return gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, YESNO_TMO, NULL,
                                   main_message, yes_message, no_message);
}

extern enum yesno_res gui_syncyesno_run_w_title(
                                 const char * title,
                                 const struct text_message * main_message,
                                 const struct text_message * yes_message,
                                 const struct text_message * no_message)
{
    return gui_syncyesno_run_w_tmo(TIMEOUT_BLOCK, YESNO_TMO, title,
                                   main_message, yes_message, no_message);
}

static bool yesno_pop_lines(const char *lines[], int line_cnt)
{
    const struct text_message message={lines, line_cnt};
    bool ret = (gui_syncyesno_run(&message,NULL,NULL)== YESNO_YES);
    if (!dialog_is_classic())
        FOR_NB_SCREENS(i)
            screens[i].clear_viewport();
    return ret;
}

/* YES/NO dialog, uses text parameter as prompt */
bool yesno_pop(const char* text)
{
    const char *lines[]= {text};
    return yesno_pop_lines(lines, 1);
}

/* YES/NO dialog, asks "Are you sure?", displays
   text parameter on second line.

   Says "Cancelled" if answered negatively.
*/
bool yesno_pop_confirm(const char* text)
{
    bool confirmed;
    const char *lines[] = {ID2P(LANG_ARE_YOU_SURE), text};
    confirmed = yesno_pop_lines(lines, 2);

    if (!confirmed)
        splash(HZ, ID2P(LANG_CANCEL));

    return confirmed;
}
