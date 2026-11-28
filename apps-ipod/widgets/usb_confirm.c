/* PodBox USB consent popup. GNU General Public License, version 2+.
 * Uses the shared popup frame over the current screen; playback stays live. */
#include "config.h"
#include "ui/native_theme_runtime.h"
#include "usb.h"
#ifdef HAVE_USB_CONFIRM
#include "widgets/usb_confirm.h"
#include "plugins/rock_pitch.h"
#include "widgets/dialog.h"
#include "ui/scene.h"
#include "input/action.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "screens/playback/wps.h"
#include "screens/playback/handoff.h"
#include "audio.h"
#include "backlight.h"
#include "font.h"
#ifdef HAVE_USB_PODLINK
#include "screens/playback/handoff_sheet.h"
#endif

static const char *const lines[] = {
    "Computer connected",
    "SELECT  Connect",
    "MENU    Charge only",
};

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
    box->width = MIN(width + in.left + in.right, s->lcdwidth - 2 * d->style.edge_margin);
    box->height = ARRAYLEN(lines) * s->getcharheight() + in.top + in.bottom;
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
}


static int confirm_action(struct dialog *d, int action, void *data)
{
    uint32_t epoch = *(uint32_t *)data;
    if (!usb_confirm_pending(epoch))
        return DIALOG_ABORT;
    if (handoff_prompt_wps(epoch))
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

#define USB_POPUP_ROW_GAP 6

static void confirm_classic_measure(struct dialog *d, struct screen *s,
                                    struct viewport *box, void *data)
{
    struct usb_classic_ctx *c = data;
    int hfont = c->head_font >= 0 ? c->head_font : box->font;
    int hch = font_get(hfont)->height;
    int bch = font_get(box->font)->height;
    int rows_h = 2 * DIALOG_POPUP_BUTTON_H + USB_POPUP_ROW_GAP;
    int box_w = MIN(s->lcdwidth - 40, 272);
    int box_h = 14 + hch + 6 + 2 * bch + 2 + 12 + rows_h + 12;
    if (box_h > s->lcdheight - 20)
        box_h = s->lcdheight - 20;

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
    int hfont = c->head_font >= 0 ? c->head_font : content->font;
    int lfont = c->label_font >= 0 ? c->label_font : content->font;
    int kfont = content->font;
    int hch = font_get(hfont)->height;
    int bch = font_get(content->font)->height;
    int old_font = content->font;
    int rows_h = 2 * DIALOG_POPUP_BUTTON_H + USB_POPUP_ROW_GAP;
    int ry = content->height - 12 - rows_h;
    unsigned shadow = LCD_RGBPACK(4, 10, 24);
    unsigned body_fg = LCD_RGBPACK(234, 240, 250);
    const char *title = "Computer Connected";
    const char *sub1 = "Connect as a USB disk to transfer files,";
    const char *sub2 = "or charge while keeping playback.";
    int tw = 0;

    s->set_drawmode(DRMODE_FG);
    s->setfont(hfont);
    s->getstringsize((const unsigned char *)title, &tw, NULL);
    dialog_draw_shadow_text(s, MAX(12, (content->width - tw) / 2), 14,
                            title, LCD_RGBPACK(255, 255, 255), shadow, -1);
    s->setfont(old_font);

    s->getstringsize((const unsigned char *)sub1, &tw, NULL);
    dialog_draw_shadow_text(s, MAX(12, (content->width - tw) / 2),
                            14 + hch + 6, sub1, body_fg, shadow, -1);
    s->getstringsize((const unsigned char *)sub2, &tw, NULL);
    dialog_draw_shadow_text(s, MAX(12, (content->width - tw) / 2),
                            14 + hch + 6 + bch + 2, sub2, body_fg, shadow, -1);

    int row_w = content->width - 24;
    dialog_draw_popup_action_row(s, 12, ry, row_w, DIALOG_POPUP_BUTTON_H,
                                 "MENU", false, "Charge Only",
                                 kfont, lfont);
    ry += DIALOG_POPUP_BUTTON_H + USB_POPUP_ROW_GAP;
    dialog_draw_popup_action_row(s, 12, ry, row_w, DIALOG_POPUP_BUTTON_H,
                                 "SELECT", false, "Connect",
                                 kfont, lfont);
    s->set_drawmode(DRMODE_SOLID);
}

static int confirm_classic_action(struct dialog *d, int action, void *data)
{
    struct usb_classic_ctx *c = data;
    return confirm_action(d, action, &c->epoch);
}

/* The UI thread retains the request while boot notices or the first Home
 * paint own the screen. The USB thread still owns the cable epoch and reply. */
static bool boot_ready;
static uint32_t deferred_epoch;

bool gui_usb_confirm_boot_waiting(void)
{
    return !boot_ready;
}

bool gui_usb_confirm_boot_pending(void)
{
    return deferred_epoch && usb_confirm_pending(deferred_epoch);
}

void gui_usb_confirm_boot_ready(void)
{
    boot_ready = true;
}

bool gui_usb_confirm_run_deferred(void)
{
    if (!boot_ready || !deferred_epoch)
        return false;

    uint32_t epoch = deferred_epoch;
    deferred_epoch = 0;
    return gui_usb_confirm_run(epoch);
}

bool gui_usb_confirm_run(uint32_t epoch)
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
        return false;
    if (!boot_ready)
    {
        deferred_epoch = epoch;
        return false;
    }

    ui_scene_interrupt();
    dialog_interrupt_transition();
    rock_pitch_revoke();
    if (handoff_prompt_wps(epoch))
        return true;
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
    return true;
}
#endif
