/* Indexed SDL display and iPod input for Quake. GPL-2.0-or-later. */
#include "quakedef.h"
#include "usb.h"

static Uint8 indices[LCD_HEIGHT][LCD_WIDTH];
static fb_data pixels[LCD_HEIGHT][LCD_WIDTH];
static fb_data palette[256];
static SDL_Surface display;
static SDL_Event events[32];
static unsigned int first, queued;
static bool active;
static unsigned int held_buttons;
#ifdef HAS_BUTTON_HOLD
static bool held_hold;
#endif

/* The click wheel pad of Rockbox's SDL port: Menu and Play are up and
 * down, Select is Enter and the wheel strafes with A and D. */
static const struct
{
    unsigned int button;
    SDLKey key;
} pad[] = {
    { BUTTON_SELECT, SDLK_RETURN },
    { BUTTON_MENU, SDLK_UP },
    { BUTTON_PLAY, SDLK_DOWN },
    { BUTTON_LEFT, SDLK_LEFT },
    { BUTTON_RIGHT, SDLK_RIGHT },
};

static struct strafe
{
    SDLKey key;
    bool held;
    long release;
} strafe[] = { { .key = SDLK_a }, { .key = SDLK_d } };

int quake_sdl_init(unsigned int flags)
{
    if (!(flags & SDL_INIT_VIDEO))
        return -1;

    active = true;
    rb->lcd_set_viewport(NULL);
    rb->lcd_set_backdrop(NULL);
    rb->backlight_set_timeout(0);
    rb->backlight_set_timeout_plugged(0);
    return 0;
}

void quake_sdl_quit(void)
{
    active = false;
}

SDL_Surface *quake_sdl_mode(int w, int h, int bits, unsigned int flags)
{
    (void)flags;
    if (!active || w != LCD_WIDTH || h != LCD_HEIGHT || bits != 8)
        return NULL;
    display = (SDL_Surface){w, h, LCD_WIDTH, indices};
    return &display;
}

void quake_sdl_palette(SDL_Surface *surface, SDL_Color *colors, int start,
                       int count)
{
    if (!active || surface != &display || start < 0 || start > 256 ||
        count < 0 || count > 256 - start)
        return;

    for (int i = 0; i < count; i++)
        palette[start + i] = LCD_RGBPACK(colors[i].r, colors[i].g, colors[i].b);
}

void quake_sdl_update(SDL_Surface *surface, int x, int y, int w, int h)
{
    if (!active || surface != &display || x < 0 || y < 0 ||
        x >= LCD_WIDTH || y >= LCD_HEIGHT || w <= 0 || h <= 0)
        return;

    w = MIN(w, LCD_WIDTH - x);
    h = MIN(h, LCD_HEIGHT - y);
    for (int row = y; row < y + h; row++)
    {
        for (int col = x; col < x + w; col++)
            pixels[row][col] = palette[indices[row][col]];
        if ((row & 31) == 31)
            quake_check_events();
    }
    rb->lcd_bitmap_part(&pixels[0][0], x, y, LCD_WIDTH, x, y, w, h);
    rb->lcd_update_rect(x, y, w, h);
}

static void add_key(SDLKey key, bool down)
{
    if (active && queued < ARRAYLEN(events))
    {
        unsigned int next = (first + queued++) % ARRAYLEN(events);
        events[next] = (SDL_Event) {
            .type = down ? SDL_KEYDOWN : SDL_KEYUP,
            .key = { .keysym = { key }, .state = down }
        };
    }
}

bool quake_system_event(long button)
{
    if (button != SYS_USB_CONNECTED && button != SYS_USB_CONFIRM &&
        !(button & SYS_EVENT))
        return false;

    long result = rb->default_event_handler(button);
    if (result == SYS_USB_CONNECTED)
        quake_quit(PLUGIN_USB_CONNECTED);
    if (result == SYS_POWEROFF || result == SYS_REBOOT)
        quake_quit(PLUGIN_POWEROFF);
    return true;
}

/* Each wheel step holds a strafe key, which lifts HZ / 25 after the last
 * step in that direction. */
static void strafe_step(struct strafe *strafe, long now)
{
    if (!strafe->held)
        add_key(strafe->key, true);
    strafe->held = true;
    strafe->release = now + HZ / 25;
}

void quake_check_events(void)
{
    static long last_poll = LONG_MIN;
    long now = *rb->current_tick;
    if (now == last_poll)
        return;
    last_poll = now;

    /* The queue only carries the wheel, remote volume and system events;
     * the buttons below are read as held state. */
    long button;
    while ((button = rb->button_get(false)) != BUTTON_NONE)
    {
        if (quake_system_event(button) || (button & BUTTON_REL))
            continue;
        switch (button & ~BUTTON_REPEAT)
        {
            case BUTTON_SCROLL_BACK:
                strafe_step(&strafe[0], now);
                break;
            case BUTTON_SCROLL_FWD:
                strafe_step(&strafe[1], now);
                break;
            case BUTTON_MULTIMEDIA_VOLUME_UP:
#ifdef BUTTON_RC_VOL_UP
            case BUTTON_RC_VOL_UP:
#endif
                rb->adjust_volume(1);
                break;
            case BUTTON_MULTIMEDIA_VOLUME_DOWN:
#ifdef BUTTON_RC_VOL_DOWN
            case BUTTON_RC_VOL_DOWN:
#endif
                rb->adjust_volume(-1);
                break;
        }
    }
    for (unsigned int i = 0; i < ARRAYLEN(strafe); i++)
    {
        if (strafe[i].held && TIME_AFTER(now, strafe[i].release))
        {
            add_key(strafe[i].key, false);
            strafe[i].held = false;
        }
    }

#ifdef HAS_BUTTON_HOLD
    /* Hold on presses Escape and Hold off releases it. */
    bool hold = rb->button_hold();
    if (hold != held_hold)
        add_key(SDLK_ESCAPE, hold);
    held_hold = hold;
#endif

    unsigned int buttons = rb->button_status();
    unsigned int released = ~buttons & held_buttons;
    unsigned int pressed = buttons & ~held_buttons;
    held_buttons = buttons;
    for (unsigned int i = 0; i < ARRAYLEN(pad); i++)
        if (released & pad[i].button)
            add_key(pad[i].key, false);
    for (unsigned int i = 0; i < ARRAYLEN(pad); i++)
        if (pressed & pad[i].button)
            add_key(pad[i].key, true);
}

int quake_sdl_poll(SDL_Event *event)
{
    quake_check_events();
    if (!queued)
        return 0;
    *event = events[first];
    first = (first + 1) % ARRAYLEN(events);
    queued--;
    return 1;
}
