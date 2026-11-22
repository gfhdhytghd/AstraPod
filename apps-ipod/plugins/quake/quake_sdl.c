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
static SDLKey held_strafe;
static long strafe_tick;

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
    if (queued < ARRAYLEN(events))
    {
        unsigned int next = (first + queued++) % ARRAYLEN(events);
        events[next] = (SDL_Event) {
            .type = down ? SDL_KEYDOWN : SDL_KEYUP,
            .key = { .keysym = { key }, .state = down }
        };
    }
}

void quake_check_events(void)
{
    static long last_poll = LONG_MIN;
    long now = *rb->current_tick;
    if (now == last_poll)
        return;
    last_poll = now;

    /* The wheel holds a strafe key until it stops turning. */
    if (held_strafe && (key_dest != key_game || now - strafe_tick > HZ / 8) &&
        queued < ARRAYLEN(events))
    {
        add_key(held_strafe, false);
        held_strafe = 0;
    }

    unsigned int budget = ARRAYLEN(events);
    while (budget--)
    {
        long button = rb->button_get(false);
        if (button == BUTTON_NONE)
            return;

        if (button == SYS_USB_CONNECTED || button == SYS_USB_CONFIRM ||
            (button & SYS_EVENT))
        {
            long result = rb->default_event_handler(button);
            if (result == SYS_USB_CONNECTED)
                quake_quit(PLUGIN_USB_CONNECTED);
            if (result == SYS_POWEROFF || result == SYS_REBOOT)
                quake_quit(PLUGIN_POWEROFF);
            continue;
        }

        bool released = (button & BUTTON_REL) != 0;
        bool repeat = (button & BUTTON_REPEAT) != 0;
        long key = button & ~(BUTTON_REL | BUTTON_REPEAT);
        if (!released || repeat)
        {
            switch (key)
            {
                case BUTTON_MULTIMEDIA_VOLUME_UP:
#ifdef BUTTON_RC_VOL_UP
                case BUTTON_RC_VOL_UP:
#endif
                    rb->adjust_volume(1);
                    continue;
                case BUTTON_MULTIMEDIA_VOLUME_DOWN:
#ifdef BUTTON_RC_VOL_DOWN
                case BUTTON_RC_VOL_DOWN:
#endif
                    rb->adjust_volume(-1);
                    continue;
            }
        }

        if (key == BUTTON_MENU && repeat && !released)
            quake_quit(PLUGIN_OK);

        SDLKey sym;
        switch (key)
        {
            case BUTTON_MENU: sym = SDLK_ESCAPE; break;
            case BUTTON_SELECT: sym = SDLK_RETURN; break;
            case BUTTON_LEFT: sym = SDLK_LEFT; break;
            case BUTTON_RIGHT: sym = SDLK_RIGHT; break;
            case BUTTON_PLAY: sym = SDLK_UP; break;
            case BUTTON_SCROLL_BACK: sym = SDLK_UP; break;
            case BUTTON_SCROLL_FWD: sym = SDLK_DOWN; break;
            default: continue;
        }
        if (!active)
            continue;
        if ((key == BUTTON_SCROLL_BACK || key == BUTTON_SCROLL_FWD) &&
            key_dest == key_game)
        {
            SDLKey strafe = key == BUTTON_SCROLL_BACK ? SDLK_a : SDLK_d;
            if (released)
                continue;
            if (held_strafe != strafe && queued + 2 <= ARRAYLEN(events))
            {
                if (held_strafe)
                    add_key(held_strafe, false);
                add_key(strafe, true);
                held_strafe = strafe;
            }
            strafe_tick = now;
        }
        else if (key == BUTTON_SCROLL_BACK || key == BUTTON_SCROLL_FWD)
        {
            if (!released && queued + 2 <= ARRAYLEN(events))
            {
                add_key(sym, true);
                add_key(sym, false);
            }
        }
        else
            add_key(sym, !released);
    }
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
