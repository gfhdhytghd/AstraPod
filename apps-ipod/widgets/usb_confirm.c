/* PodBox USB consent popup. GNU General Public License, version 2+.
 * Uses the shared popup frame over the current screen; playback stays live. */
#include "config.h"
#include "ui/native_theme_runtime.h"
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
#include "ui/statusbar.h"
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

static const char *remote_hint(void)
{
    return ui_wheel_navigation() ? "Hold SELECT   Remote control" :
                                   (const char *)str(LANG_HANDOFF_USB);
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
        s->getstringsize(remote_hint(), &w, NULL);
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
        s->putsxy(0, ARRAYLEN(lines) * s->getcharheight(), remote_hint());
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
    static const struct button_mapping wheel_keys[] = {
        { ACTION_STD_OK, BUTTON_SELECT | BUTTON_REL, BUTTON_SELECT },
        { ACTION_STD_NEXT, BUTTON_SELECT | BUTTON_REPEAT, BUTTON_SELECT },
        { ACTION_STD_CANCEL, BUTTON_MENU, BUTTON_NONE },
        LAST_ITEM_IN_LIST
    };
    if (!(context & CONTEXT_REMOTE) && ui_wheel_navigation())
        return wheel_keys;

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
    int sel;
    int head_font;
    int row_font;
};

static int usb_classic_count(void)
{
    return remote_choice() ? 3 : 2;
}

static void confirm_classic_measure(struct dialog *d, struct screen *s,
                                    struct viewport *box, void *data)
{
    (void)data;
    int sh = sb_visible_height();
    if (sh <= 0)
        sh = (int)ui_theme_metric(UI_METRIC_STATUS_HEIGHT);
    d->style.box_margin = 0;
    box->x = 0;
    box->y = sh;
    box->width = s->lcdwidth;
    box->height = MAX(1, s->lcdheight - sh);
}

static void confirm_classic_draw(struct dialog *d, struct screen *s,
                                 struct viewport *content, void *data)
{
    (void)d;
    struct usb_classic_ctx *c = data;
    int count = usb_classic_count();
    if (c->sel >= count)
        c->sel = count - 1;

    int hfont = c->head_font >= 0 ? c->head_font : content->font;
    int rfont = c->row_font >= 0 ? c->row_font : content->font;
    int bch = font_get(content->font)->height;
    int old_font = content->font;

    s->set_drawmode(DRMODE_FG);
    s->set_foreground(LCD_RGBPACK(0, 0, 0));
    s->setfont(hfont);
    s->putsxy(22, 22, (const unsigned char *)"Computer Connected");
    s->setfont(old_font);

    static const char *const desc[2] = {
        "Connect as a USB disk to transfer files,",
        "or charge only while keeping playback.",
    };
    s->putsxy(22, 48, (const unsigned char *)desc[0]);
    s->putsxy(22, 48 + bch + 2, (const unsigned char *)desc[1]);

    int row_h = (int)ui_theme_metric(UI_METRIC_ROW_HEIGHT);
    if (row_h < 20)
        row_h = 24;
    int list_y = content->height - count * row_h - 24;
    if (list_y < 96)
        list_y = 96;

    s->set_foreground(LCD_RGBPACK(208, 208, 208));
    s->hline(0, content->width - 1, list_y - 1);

    static const char *const choices[3] = {
        "Connect",
        "Charge Only",
        "Remote Control",
    };
    for (int i = 0; i < count; i++)
        dialog_draw_stock_row(s, 0, list_y + i * row_h, content->width, row_h,
                              choices[i], i == c->sel, i != 1, rfont);
    s->set_drawmode(DRMODE_SOLID);
}

static int confirm_classic_action(struct dialog *d, int action, void *data)
{
    struct usb_classic_ctx *c = data;
    if (!usb_confirm_pending(c->epoch))
        return DIALOG_ABORT;
    if (remote_choice() && handoff_prompt_wps(c->epoch))
        return DIALOG_ACCEPT;
    int count = usb_classic_count();
    switch (action)
    {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            if (c->sel > 0)
                c->sel--;
            break;
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            if (c->sel + 1 < count)
                c->sel++;
            break;
        case ACTION_STD_OK:
            if (!d->backlight_on)
                break;
            if (c->sel == 2 && remote_choice())
            {
                usb_confirm_reply(c->epoch, false);
                handoff_begin();
            }
            else
            {
                handoff_usb_dismissed();
                usb_confirm_reply(c->epoch, c->sel == 0);
            }
            return DIALOG_ACCEPT;
        case ACTION_STD_CANCEL:
            if (!d->backlight_on)
                break;
            handoff_usb_dismissed();
            usb_confirm_reply(c->epoch, false);
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
        case ACTION_WPS_SKIPPREV:
            if (audio_status() & AUDIO_STATUS_PLAY)
                audio_prev();
            break;
        case ACTION_WPS_SKIPNEXT:
            if (audio_status() & AUDIO_STATUS_PLAY)
                audio_next();
            break;
        default:
            if (action & (SYS_EVENT | BUTTON_MULTIMEDIA))
                default_event_handler(action);
            break;
    }
    return DIALOG_CONTINUE;
}

static const struct button_mapping *confirm_classic_keymap(int context)
{
    (void)context;
    static const struct button_mapping fruit_keys[] = {
        { ACTION_STD_PREV,      BUTTON_SCROLL_BACK,                BUTTON_NONE },
        { ACTION_STD_PREV,      BUTTON_SCROLL_BACK|BUTTON_REPEAT,  BUTTON_NONE },
        { ACTION_STD_NEXT,      BUTTON_SCROLL_FWD,                 BUTTON_NONE },
        { ACTION_STD_NEXT,      BUTTON_SCROLL_FWD|BUTTON_REPEAT,   BUTTON_NONE },
        { ACTION_STD_OK,        BUTTON_SELECT|BUTTON_REL,          BUTTON_SELECT },
        { ACTION_STD_CANCEL,    BUTTON_MENU|BUTTON_REL,            BUTTON_MENU },
        { ACTION_WPS_PLAY,      BUTTON_PLAY|BUTTON_REL,            BUTTON_PLAY },
        { ACTION_WPS_SKIPPREV,  BUTTON_LEFT|BUTTON_REL,            BUTTON_LEFT },
        { ACTION_WPS_SKIPNEXT,  BUTTON_RIGHT|BUTTON_REL,           BUTTON_RIGHT },
#ifdef IPOD_ACCESSORY_PROTOCOL
        { ACTION_STD_CANCEL,    BUTTON_RC_MENU,                    BUTTON_NONE },
        { ACTION_STD_OK,        BUTTON_RC_PLAY|BUTTON_REL,         BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_PLAY|BUTTON_REPEAT,    BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_STOP,                  BUTTON_NONE },
        { BUTTON_MULTIMEDIA_NEXT, BUTTON_RC_RIGHT|BUTTON_REL,      BUTTON_RC_RIGHT },
        { BUTTON_MULTIMEDIA_PREV, BUTTON_RC_LEFT|BUTTON_REL,       BUTTON_RC_LEFT },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP,           BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP|BUTTON_REPEAT, BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN,       BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN|BUTTON_REPEAT, BUTTON_NONE },
#endif
        LAST_ITEM_IN_LIST
    };
    if (ui_theme_style(UI_STYLE_MENU_TREE) == UI_MENU_TREE_STOCK)
        return fruit_keys;

    static const struct button_mapping classic_keys[] = {
        { ACTION_STD_PREV,      BUTTON_SCROLL_BACK,                BUTTON_NONE },
        { ACTION_STD_PREV,      BUTTON_SCROLL_BACK|BUTTON_REPEAT,  BUTTON_NONE },
        { ACTION_STD_NEXT,      BUTTON_SCROLL_FWD,                 BUTTON_NONE },
        { ACTION_STD_NEXT,      BUTTON_SCROLL_FWD|BUTTON_REPEAT,   BUTTON_NONE },
        { ACTION_STD_PREV,      BUTTON_LEFT,                       BUTTON_NONE },
        { ACTION_STD_NEXT,      BUTTON_RIGHT,                      BUTTON_NONE },
        { ACTION_STD_OK,        BUTTON_SELECT|BUTTON_REL,          BUTTON_SELECT },
        { ACTION_STD_CANCEL,    BUTTON_MENU|BUTTON_REL,            BUTTON_MENU },
        { ACTION_WPS_PLAY,      BUTTON_PLAY|BUTTON_REL,            BUTTON_PLAY },
#ifdef IPOD_ACCESSORY_PROTOCOL
        { ACTION_STD_CANCEL,    BUTTON_RC_MENU,                    BUTTON_NONE },
        { ACTION_STD_OK,        BUTTON_RC_PLAY|BUTTON_REL,         BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_PLAY|BUTTON_REPEAT,    BUTTON_RC_PLAY },
        { BUTTON_MULTIMEDIA_STOP, BUTTON_RC_STOP,                  BUTTON_NONE },
        { BUTTON_MULTIMEDIA_NEXT, BUTTON_RC_RIGHT|BUTTON_REL,      BUTTON_RC_RIGHT },
        { BUTTON_MULTIMEDIA_PREV, BUTTON_RC_LEFT|BUTTON_REL,       BUTTON_RC_LEFT },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP,           BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_UP, BUTTON_RC_VOL_UP|BUTTON_REPEAT, BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN,       BUTTON_NONE },
        { BUTTON_MULTIMEDIA_VOLUME_DOWN, BUTTON_RC_VOL_DOWN|BUTTON_REPEAT, BUTTON_NONE },
#endif
        LAST_ITEM_IN_LIST
    };
    return classic_keys;
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
            .sel = 0,
            .head_font = ui_theme_current_font_load(UI_FONT_TITLE, 48),
            .row_font = ui_theme_current_font_load(UI_FONT_LIST, 48),
        };
        dialog_init(&d, CONTEXT_STD, "USB", NULL, &classic_cb, &ctx);
        d.motion_route = "usb";
        d.keymap = confirm_classic_keymap;
        dialog_run(&d, HZ / 10);
        font_unload(ctx.head_font);
        font_unload(ctx.row_font);
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
