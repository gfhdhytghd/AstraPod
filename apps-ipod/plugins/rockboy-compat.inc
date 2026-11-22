/* Foreground front end for the gnuboy core. GPL-2.0-or-later. */
int shut;
static const char *errormsg;

struct options options;
static unsigned char *arena;
static size_t remaining;

void die(char *message, ...)
{
    shut = 1;
    errormsg = message;
}

void *my_malloc(size_t size)
{
    if (size > remaining)
        return NULL;
    size_t bytes = ALIGN_UP(size, sizeof(long));
    if (bytes > remaining)
        return NULL;
    void *result = arena;
    arena += bytes;
    remaining -= bytes;
    return result;
}

void doevents(void)
{
    event_t ev;
    ev_poll();
    while (ev_getevent(&ev))
    {
        if (ev.type == EV_PRESS || ev.type == EV_RELEASE)
            pad_set(ev.code, ev.type == EV_PRESS);
    }
}

int do_user_menu(void)
{
    MENUITEM_STRINGLIST(menu, "Rockboy", NULL, "Resume", "Quit");
    int selected = 0;
    int result = rb->do_menu(&menu, &selected, NULL, false);
    return result == 1 || result == MENU_ATTACHED_USB ? USER_MENU_QUIT : 0;
}

enum plugin_status plugin_start(const void *parameter)
{
    char selected[MAX_PATH];
    if (!parameter)
    {
        if (rb->pick_file(selected, sizeof(selected), "/", "Choose Game Boy ROM",
                          ".gb,.gbc", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        parameter = selected;
    }

    arena = rb->plugin_claim_scratch(128 * 1024, SIZE_MAX, &remaining);
    if (!arena)
        return PLUGIN_ERROR;

    loader_init(parameter);
    if (shut)
    {
        rb->splashf(HZ * 2, "%s", errormsg);
        return PLUGIN_ERROR;
    }

    options = (struct options){
        .UP = BUTTON_SCROLL_BACK, .DOWN = BUTTON_SCROLL_FWD,
        .LEFT = BUTTON_LEFT, .RIGHT = BUTTON_RIGHT,
        .A = BUTTON_SELECT, .B = BUTTON_SELECT | BUTTON_LEFT,
        .START = BUTTON_PLAY,
        .SELECT = BUTTON_SELECT | BUTTON_RIGHT,
        .maxskip = 4, .sound = 1, .scaling = 3,
    };
    rockboy_pcm_init();
    if (!pcm.buf)
        options.sound = 0;
    vid_init();
    emu_reset();
    backlight_ignore_timeout();
    rb->lcd_clear_display();
    emu_run();
    rockboy_pcm_close();
    backlight_use_settings();
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (errormsg)
    {
        rb->splashf(HZ * 2, "%s", errormsg);
        return PLUGIN_ERROR;
    }
    return PLUGIN_OK;
}
