/***************************************************************************
 * Original code from RockBox
 * was: apps/recorder/keyboard.c
 * Copyright (C) 2002 by Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * On-screen keyboard. dialog_input() edits a string in a boxed single-line
 * editor driven by the click wheel.
 ****************************************************************************/

/* The editor itself -- the caret model, the wheel and the rendering -- is
 * widgets/edit_line.c, shared with the database search. What is here is the
 * enclosure: a dialog box holding the line above a Cancel/OK row, the focus
 * that moves between the two, and the accept/discard semantics.
 *
 * This is an iPod-only build, so the old keyboard's loadable layouts, morse
 * input and touchscreen grid are intentionally gone. */

#include "draw/screen_access.h"
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "system.h"
#include "system/shutdown.h"
#include "settings/settings.h"    /* ID2P */
#include "rbunicode.h"
#include "input/action.h"
#include "lang.h"
#include "keyboard.h"
#include "draw/viewport.h"
#include "yesno.h"
#include "dialog.h"
#include "edit_line.h"
#include "speech/talk.h"

struct kbd_edit {
    struct edit_line ed;
    bool on_buttons;     /* focus is on the Cancel/OK button row, not the input */
    bool btn_ok;         /* on the button row, OK is selected (else Cancel) */
    bool dirty;          /* text changed since the editor opened */
};

/* The dialog is centred in display coordinates. */
static void kbd_measure(struct dialog *d, struct screen *display,
                        struct viewport *box, void *data)
{
    (void)data;
    struct dialog_insets in;
    int area_h = display->lcdheight;
    int sw = display->lcdwidth;
    int ch_h = display->getcharheight();

    dialog_get_insets(&d->style, &in);

    int bh = ch_h + 2 * d->style.button_pad_y;      /* button height */
    int gap = ch_h / 2;                     /* gap between edit line and buttons */
    int box_h = ch_h + gap + bh + in.top + in.bottom;
    if (box_h > area_h)
        box_h = area_h;

    box->x = d->style.edge_margin;
    box->width = sw - 2 * d->style.edge_margin;
    box->y = (area_h - box_h) / 2;
    box->height = box_h;
}

static void kbd_draw(struct dialog *d, struct screen *display,
                     struct viewport *content, void *data)
{
    struct kbd_edit *s = data;
    int ch_h = display->getcharheight();
    int bh = ch_h + 2 * d->style.button_pad_y;      /* button height */

    /* --- edit line in a clipped sub-viewport so long text can scroll --- */
    struct viewport tvp = *content;
    tvp.height = MIN(content->height, edit_line_height(display));
    display->set_viewport(&tvp);
    edit_line_draw(display, &s->ed, tvp.width);

    /* --- bottom Cancel / OK buttons (triggered by MENU / SELECT) --- */
    display->set_viewport(content);
    display->set_drawmode(DRMODE_SOLID);

    int by = content->height - bh;
    int bw = (content->width - d->style.button_gap) / 2;
    if (bw > 0)
    {
        dialog_draw_button(display, &d->style, 0, by, bw, bh,
                           str(LANG_DIAG_CANCEL),
                           s->on_buttons && !s->btn_ok);
        dialog_draw_button(display, &d->style, bw + d->style.button_gap, by, bw, bh,
                           str(LANG_DIAG_OK),
                           s->on_buttons && s->btn_ok);
    }

    display->set_drawmode(DRMODE_SOLID);
}

/* Confirm discarding edits (only if the text changed); returns the disposition
 * dialog_run() should act on. */
static int kbd_confirm_cancel(struct kbd_edit *s)
{
    if (s->dirty)
    {
        const char *lines[] = {ID2P(LANG_KBD_DISCARD)};
        struct text_message message = {lines, 1};
        if (gui_syncyesno_run(&message, NULL, NULL) != YESNO_YES)
        {
            s->on_buttons = false;   /* keep editing, back to the input */
            return DIALOG_CONTINUE;
        }
    }
    return DIALOG_CANCEL;
}

static void kbd_speak_button(const struct kbd_edit *s)
{
    if (global_settings.talk_menu)
        talk_id(s->btn_ok ? LANG_DIAG_OK : LANG_DIAG_CANCEL, false);
}

static int kbd_on_action(struct dialog *d, int action, void *data)
{
    (void)d;
    struct kbd_edit *s = data;

    /* On the button row the wheel and the taps mean the buttons, not the text,
     * so the line only sees them while the input has focus. */
    if (edit_line_owns_action(action))
    {
        if (!s->on_buttons)
        {
            if (edit_line_action(&s->ed, action))
                s->dirty = true;
        }
        else switch (action)
        {
            case ACTION_KBD_UP:
            case ACTION_KBD_DOWN:
                s->btn_ok = !s->btn_ok;         /* toggle OK/Cancel */
                kbd_speak_button(s);
                break;
            case ACTION_KBD_LEFT:
                s->btn_ok = false;              /* Cancel (left button) */
                kbd_speak_button(s);
                break;
            case ACTION_KBD_RIGHT:
                s->btn_ok = true;               /* OK (right button) */
                kbd_speak_button(s);
                break;
        }
        return DIALOG_CONTINUE;
    }

    switch (action)
    {
        case ACTION_KBD_DONE:      /* PLAY: move focus down to the buttons */
            if (!s->on_buttons)
            {
                s->on_buttons = true;
                s->btn_ok = true;               /* start on OK */
                kbd_speak_button(s);
            }
            break;
        case ACTION_KBD_SPEAK:     /* SELECT held: read back the text */
            edit_line_speak(&s->ed);
            break;
        case ACTION_KBD_SELECT:    /* accept, or confirm the chosen button */
            if (s->on_buttons && !s->btn_ok)
                return kbd_confirm_cancel(s);   /* Cancel button */
            return DIALOG_ACCEPT;               /* OK / input shortcut */
        case ACTION_KBD_ABORT:     /* MENU: up to the input, or cancel */
            if (s->on_buttons)
            {
                s->on_buttons = false;          /* back up to the input */
                edit_line_speak(&s->ed);
                break;
            }
            return kbd_confirm_cancel(s);
        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

/* Click-wheel single-line text editor. Returns 0 on accept (buffer updated),
 * -1 on cancel or USB (buffer left unchanged). */
int dialog_input(char* text, int buflen)
{
    static const struct dialog_callbacks cb = {
        .measure   = kbd_measure,
        .draw      = kbd_draw,
        .on_action = kbd_on_action,
    };
    struct dialog d;

    int handle = core_alloc_free_space(sizeof(struct kbd_edit));
    if (handle <= 0)
        return -1;

    struct kbd_edit *state = core_get_data_pinned(handle);
    *state = (struct kbd_edit){.btn_ok = true};
    edit_line_init(&state->ed, text, buflen - 1);

    dialog_init(&d, CONTEXT_KEYBOARD, NULL, NULL, &cb, state);
    edit_line_speak(&state->ed);
    bool accepted = dialog_run(&d, TIMEOUT_BLOCK) == DIALOG_ACCEPT;
    if (accepted)
        edit_line_get(&state->ed, text, buflen, true);

    core_put_data_pinned(state);
    core_free(handle);
    return accepted ? 0 : -1;
}

/* Plugin-ABI wrapper: the loadable-layout `kbd` argument is ignored. */
int kbd_input(char* text, int buflen, ucschar_t *kbd)
{
    (void)kbd;
    return dialog_input(text, buflen);
}
