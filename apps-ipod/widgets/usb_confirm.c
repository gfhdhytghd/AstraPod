/* PodBox USB consent popup. GNU General Public License, version 2+.
 * Uses the shared popup frame over the current screen; playback stays live. */
#include "config.h"
#include "usb.h"
#ifdef HAVE_USB_CONFIRM
#include "widgets/usb_confirm.h"
#include "plugins/rock_pitch.h"
#include "widgets/dialog.h"
#include "input/action.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "screens/playback/wps.h"
#include "screens/playback/handoff.h"
#include "screens/playback/cider.h"
#include "lang.h"
#include "audio.h"
#include "backlight.h"
#include "font.h"
#include "ui/native_theme_runtime.h"
#include <string.h>
#ifdef HAVE_USB_PODLINK
#include "screens/playback/handoff_sheet.h"
#endif

static const char *const lines[] = {
    "Computer connected",
    "SELECT  Connect",
    "MENU    Charge only",
};

static bool remote_choice(void)
{
    return handoff_capable() && !cider_active() &&
           (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) == AUDIO_STATUS_PLAY;
}

static void confirm_measure(struct dialog *d, struct screen *s,
                            struct viewport *box, void *data)
{
    (void)data;
    struct dialog_insets in;
    int width = 0;
    dialog_get_insets(&d->style, &in);
    for (unsigned i = 0; i < ARRAYLEN(lines); ++i)
    {
        int w;
        s->getstringsize(lines[i], &w, NULL);
        width = MAX(width, w);
    }
    bool remote = remote_choice();
    if (remote)
    {
        int w;
        s->getstringsize(str(LANG_HANDOFF_USB), &w, NULL);
        width = MAX(width, w);
    }
    box->width = MIN(width + in.left + in.right, s->lcdwidth - 2 * d->style.edge_margin);
    box->height = (ARRAYLEN(lines) + remote) * s->getcharheight() + in.top + in.bottom;
    box->x = (s->lcdwidth - box->width) / 2;
    box->y = (s->lcdheight - box->height) / 2;
}

static void confirm_draw(struct dialog *d, struct screen *s,
                         struct viewport *content, void *data)
{
    (void)d;
    (void)data;
    content->flags |= VP_FLAG_ALIGN_CENTER;
    s->set_viewport(content);
    s->set_drawmode(DRMODE_FG);
    for (unsigned i = 0; i < ARRAYLEN(lines); ++i)
        s->putsxy(0, i * s->getcharheight(), lines[i]);
    if (remote_choice())
        s->putsxy(0, ARRAYLEN(lines) * s->getcharheight(), str(LANG_HANDOFF_USB));
}


static int confirm_action(struct dialog *d, int action, void *data)
{
    uint32_t epoch = *(uint32_t *)data;
    if (!usb_confirm_pending(epoch))
        return DIALOG_ABORT;
    if (remote_choice() && handoff_prompt_wps(epoch))
        return DIALOG_ACCEPT;
    switch (action)
    {
        case ACTION_STD_OK:
        case ACTION_STD_CANCEL:
            if (!d->backlight_on)
                break;
            handoff_usb_dismissed();
            usb_confirm_reply(epoch, action == ACTION_STD_OK);
            return DIALOG_ACCEPT;
        case ACTION_STD_NEXT:
            if (!d->backlight_on || !remote_choice()) break;
            usb_confirm_reply(epoch, false);
            handoff_begin();
            return DIALOG_ACCEPT;
        case ACTION_WPS_PLAY:
        case BUTTON_MULTIMEDIA_PLAYPAUSE:
            if (audio_status() & AUDIO_STATUS_PLAY)
            {
                if (audio_status() & AUDIO_STATUS_PAUSE)
                    unpause_action(true);
                else
                    pause_action(true);
            }
            break;
        default:
            if (action & (SYS_EVENT | BUTTON_MULTIMEDIA))
                default_event_handler(action);
            break;
    }
    return DIALOG_CONTINUE;
}

static const struct button_mapping *confirm_keymap(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_STD_OK, BUTTON_SELECT, BUTTON_NONE },
        { ACTION_STD_CANCEL, BUTTON_MENU, BUTTON_NONE },
        { ACTION_STD_NEXT, BUTTON_RIGHT, BUTTON_NONE },
        { ACTION_WPS_PLAY, BUTTON_PLAY | BUTTON_REL, BUTTON_PLAY },
#ifdef IPOD_ACCESSORY_PROTOCOL
        { ACTION_STD_CANCEL, BUTTON_RC_MENU, BUTTON_NONE },
        /* A remote Play tap is SELECT here: choose Disk Mode */
        { ACTION_STD_OK, BUTTON_RC_PLAY | BUTTON_REL, BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_PLAY | BUTTON_REPEAT,
          BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_STOP, BUTTON_NONE },
        { BUTTON_MULTIMEDIA_NEXT, BUTTON_RC_RIGHT | BUTTON_REL,
          BUTTON_RC_RIGHT },
        { BUTTON_MULTIMEDIA_PREV, BUTTON_RC_LEFT | BUTTON_REL,
          BUTTON_RC_LEFT },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP, BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP | BUTTON_REPEAT,
          BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN, BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN | BUTTON_REPEAT,
          BUTTON_NONE },
#endif
        LAST_ITEM_IN_LIST
    };
    return keys;
}

struct usb_classic_ctx {
    uint32_t epoch;
    int head_font;
    int label_font;
};

#define USB_POPUP_ROW_H 26

static void confirm_classic_measure(struct dialog *d, struct screen *s,
                                    struct viewport *box, void *data)
{
    struct usb_classic_ctx *c = data;
    int hfont = c->head_font >= 0 ? c->head_font : box->font;
    int hch = font_get(hfont)->height;
    int bch = font_get(box->font)->height;
    int count = remote_choice() ? 3 : 2;
    int foot_h = count * USB_POPUP_ROW_H;
    int box_w = MIN(s->lcdwidth - 40, 264);
    int box_h = 1 + 14 + hch + 5 + 2 * bch + 2 + 12 + foot_h + 1;
    if (box_h > s->lcdheight - 24)
        box_h = s->lcdheight - 24;

    d->style.box_margin = 0;
    box->width = box_w;
    box->height = box_h;
    box->x = (s->lcdwidth - box_w) / 2;
    box->y = (s->lcdheight - box_h) / 2;
}

static void confirm_classic_draw(struct dialog *d, struct screen *s,
                                 struct viewport *content, void *data)
{
    (void)d;
    struct usb_classic_ctx *c = data;
    bool remote = remote_choice();
    int count = remote ? 3 : 2;
    int hfont = c->head_font >= 0 ? c->head_font : content->font;
    int lfont = c->label_font >= 0 ? c->label_font : content->font;
    int kfont = content->font;
    int hch = font_get(hfont)->height;
    int bch = font_get(content->font)->height;
    int old_font = content->font;
    int foot_h = count * USB_POPUP_ROW_H;
    int foot_y = content->height - foot_h - 1;

    s->set_drawmode(DRMODE_FG);
    s->set_foreground(LCD_RGBPACK(0, 0, 0));
    s->setfont(hfont);
    s->putsxy(16, 15, (const unsigned char *)"Computer Connected");
    s->setfont(old_font);

    s->set_foreground(LCD_RGBPACK(56, 60, 66));
    s->putsxy(16, 15 + hch + 5,
              (const unsigned char *)"Transfer files as a USB disk, or keep");
    s->putsxy(16, 15 + hch + 5 + bch + 2,
              (const unsigned char *)"playing while charging.");

    dialog_draw_popup_footer_bg(s, 1, foot_y, content->width - 2, foot_h);
    dialog_draw_popup_action_row(s, 1, foot_y, content->width - 2,
                                 USB_POPUP_ROW_H, "SELECT", false,
                                 "Connect", true, false, kfont, lfont);
    dialog_draw_popup_action_row(s, 1, foot_y + USB_POPUP_ROW_H,
                                 content->width - 2, USB_POPUP_ROW_H,
                                 "MENU", false, "Charge Only", false, true,
                                 kfont, lfont);
    if (remote)
        dialog_draw_popup_action_row(s, 1, foot_y + 2 * USB_POPUP_ROW_H,
                                     content->width - 2, USB_POPUP_ROW_H,
                                     "RIGHT", true, "Remote Control", false,
                                     true, kfont, lfont);
    s->set_drawmode(DRMODE_SOLID);
}

static int confirm_classic_action(struct dialog *d, int action, void *data)
{
    struct usb_classic_ctx *c = data;
    return confirm_action(d, action, &c->epoch);
}

void gui_usb_confirm_run(uint32_t epoch)
{
    static const struct dialog_callbacks cb = {
        .measure = confirm_measure,
        .draw = confirm_draw,
        .on_action = confirm_action,
    };
    static const struct dialog_callbacks classic_cb = {
        .measure = confirm_classic_measure,
        .draw = confirm_classic_draw,
        .on_action = confirm_classic_action,
    };
    if (!usb_confirm_pending(epoch))
        return;
    rock_pitch_revoke();
    if (handoff_prompt_wps(epoch)) return;
#ifdef HAVE_USB_PODLINK
    handoff_sheet_interrupt();
#endif
    backlight_on();
    struct dialog d;
    if (dialog_is_classic())
    {
        struct usb_classic_ctx ctx = {
            .epoch = epoch,
            .head_font = ui_theme_current_font_load(UI_FONT_TITLE, 48),
            .label_font = ui_theme_current_font_load(UI_FONT_LIST_VALUE, 48),
        };
        dialog_init(&d, CONTEXT_STD, NULL, NULL, &classic_cb, &ctx);
        d.motion_route = "usb";
        d.keymap = confirm_keymap;
        dialog_run(&d, HZ / 10);
        font_unload(ctx.head_font);
        font_unload(ctx.label_font);
    }
    else
    {
        dialog_init(&d, CONTEXT_STD, NULL, NULL, &cb, &epoch);
        d.motion_route = "usb";
        d.keymap = confirm_keymap;
        dialog_run(&d, HZ / 10);
    }
#ifdef IPOD_6G
    if (global_settings.animations)
        action_wait_for_release_preserve_queue();
    else
#endif
        action_wait_for_release();
}
#endif
