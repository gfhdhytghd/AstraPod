/* Foreground SDL 1.2 hooks for Duke3D's indexed renderer.
 * GNU General Public License, version 2 or later.
 */
#include "SDL.h"

#define DUKE_WIDTH 320
#define DUKE_HEIGHT 200

static Uint8 pixels[DUKE_WIDTH * DUKE_HEIGHT];
static fb_data *scanout;
static SDL_Color colors[256];
static fb_data rgb[256];
static SDL_Palette palette = { .ncolors = 256, .colors = colors };
static SDL_PixelFormat format = { .palette = &palette, .BitsPerPixel = 8,
                                  .BytesPerPixel = 1 };
static SDL_Surface screen;
static SDL_VideoInfo video = { .vfmt = &format };
static SDL_GrabMode grab;
static const char *error;
static SDLKey wheel_key;
static unsigned long wheel_until;
static bool servicing;
static bool menu_chord;
static unsigned long escape_until;

static struct {
    unsigned button;
    SDLKey key;
    bool down;
} keys[] = {
    {BUTTON_LEFT, SDLK_LEFT, false},
    {BUTTON_RIGHT, SDLK_RIGHT, false},
    {BUTTON_SELECT, SDLK_RETURN, false},
    {BUTTON_PLAY, SDLK_DOWN, false},
};

#undef exit
extern void exit(int status) NORETURN_ATTR;

void duke_exit(int status)
{
    exit(status);
}

static void duke_buttons(void)
{
    for (int i = 0; i < 16; i++)
    {
        long event = rb->button_get(false);
        if (event == BUTTON_NONE)
            break;

        if (IS_SYSEVENT(event))
        {
            long result = rb->default_event_handler(event);
            if (result == SYS_USB_CONNECTED || rb->plugin_usb_taken())
                duke_exit(PLUGIN_USB_CONNECTED);
            if (result == SYS_POWEROFF || result == SYS_REBOOT)
                duke_exit(PLUGIN_POWEROFF);
            continue;
        }
        if (event & BUTTON_REL)
        {
            if (event & BUTTON_MENU)
            {
                if (!menu_chord)
                    escape_until = (unsigned long)*rb->current_tick + HZ / 10;
                menu_chord = false;
            }
            continue;
        }

        unsigned base = event & ~BUTTON_REPEAT;
        unsigned held = rb->button_status();
        const unsigned quit = BUTTON_MENU | BUTTON_SELECT;
        if (((held | base) & quit) == quit)
            duke_exit(0);
#ifdef BUTTON_RC_VOL_UP
        if (base == BUTTON_RC_VOL_UP || base == BUTTON_RC_VOL_DOWN)
        {
            rb->adjust_volume(base == BUTTON_RC_VOL_UP ? 1 : -1);
            continue;
        }
#endif
        unsigned wheel = base & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK);
        if (wheel)
        {
            bool forward = wheel == BUTTON_SCROLL_FWD;
            if ((held | base) & BUTTON_MENU)
            {
                menu_chord = true;
                rb->adjust_volume(forward ? 1 : -1);
            }
            else
            {
                bool up = forward != duke_in_menu();
                wheel_key = up ? SDLK_UP : SDLK_DOWN;
                wheel_until = (unsigned long)*rb->current_tick + HZ / 10;
            }
        }
    }
}

void duke_service(void)
{
    if (servicing)
        return;
    servicing = true;
    duke_buttons();
    duke_pcm_pump();
    rb->yield();
    servicing = false;
}

int SDL_PollEvent(SDL_Event *event)
{
    static SDLKey previous_wheel;
    duke_service();

    static bool escape_down;
    unsigned held = rb->button_status();
    unsigned long now = *rb->current_tick;
    if (TIME_AFTER(now, wheel_until))
        wheel_key = SDLK_UNKNOWN;

    SDLKey key = SDLK_UNKNOWN;
    bool down = false;
    bool escape = TIME_BEFORE(now, escape_until);
    if (escape != escape_down)
    {
        key = SDLK_ESCAPE;
        down = escape_down = escape;
    }
    else
    {
        for (size_t i = 0; i < ARRAYLEN(keys); i++)
        {
            bool pressed = (held & keys[i].button) != 0 ||
                           wheel_key == keys[i].key;
            if (pressed != keys[i].down)
            {
                keys[i].down = pressed;
                key = keys[i].key;
                down = pressed;
                break;
            }
        }
        if (key == SDLK_UNKNOWN &&
            (wheel_key == SDLK_UP) != (previous_wheel == SDLK_UP))
        {
            key = SDLK_UP;
            down = wheel_key == SDLK_UP;
            previous_wheel = down ? SDLK_UP : SDLK_UNKNOWN;
        }
    }
    if (key == SDLK_UNKNOWN)
        return 0;

    rb->memset(event, 0, sizeof(*event));
    event->type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event->key.keysym.sym = key;
    return 1;
}

Uint32 SDL_GetTicks(void)
{
    duke_service();
    return (Uint32)*rb->current_tick * (1000 / HZ);
}

void SDL_Delay(Uint32 ms)
{
    unsigned long end = (unsigned long)*rb->current_tick +
                        ms / (1000 / HZ) + 1;
    do
    {
        duke_service();
        rb->sleep(1);
    }
    while (TIME_BEFORE((unsigned long)*rb->current_tick, end));
}

int SDL_Init(Uint32 flags)
{
    (void)flags;
    return 0;
}

void duke_sdl_quit(void)
{
    screen.pixels = NULL;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    if (flags & SDL_INIT_VIDEO)
        duke_sdl_quit();
}

void SDL_Quit(void)
{
    duke_sdl_quit();
}

const SDL_version *SDL_Linked_Version(void)
{
    static const SDL_version version = {
        SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
    };
    return &version;
}

char *SDL_GetError(void)
{
    return (char *)(error ? error : "");
}

void SDL_ClearError(void)
{
    error = NULL;
}

char *SDL_VideoDriverName(char *name, int size)
{
    if (size > 0)
        strlcpy(name, "AstraPod", size);
    return name;
}

const SDL_VideoInfo *SDL_GetVideoInfo(void)
{
    return &video;
}

SDL_Rect **SDL_ListModes(SDL_PixelFormat *fmt, Uint32 flags)
{
    (void)fmt;
    (void)flags;
    static SDL_Rect mode = {0, 0, DUKE_WIDTH, DUKE_HEIGHT};
    static SDL_Rect *modes[] = {&mode, NULL};
    return modes;
}

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    (void)flags;
    if (w < 1 || w > DUKE_WIDTH || h < 1 || h > DUKE_HEIGHT || bpp != 8)
    {
        error = "Unsupported Duke3D video mode";
        return NULL;
    }

    if (!scanout)
        scanout = malloc(DUKE_WIDTH * DUKE_HEIGHT * sizeof(*scanout));
    if (!scanout)
    {
        error = "Duke3D: cannot allocate LCD buffer";
        return NULL;
    }
    rb->memset(pixels, 0, sizeof(pixels));
    screen = (SDL_Surface){
        .format = &format, .w = w, .h = h, .pitch = w, .pixels = pixels,
        .clip_rect = {0, 0, w, h},
    };
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    return &screen;
}

int SDL_SetColors(SDL_Surface *surface, SDL_Color *values, int first, int n)
{
    if (surface != &screen || first < 0 || n < 0 ||
        first > 256 || n > 256 - first)
        return 0;
    for (int i = 0; i < n; i++)
    {
        SDL_Color c = values[i];
        colors[first + i] = c;
        rgb[first + i] = LCD_RGBPACK(c.r, c.g, c.b);
    }
    return 1;
}

void SDL_UpdateRect(SDL_Surface *surface, Sint32 x, Sint32 y, Uint32 w, Uint32 h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    if (surface != &screen || !screen.pixels)
        return;

    duke_service();
    size_t count = screen.w * screen.h;
    for (size_t i = 0; i < count; i++)
        scanout[i] = rgb[pixels[i]];
    int left = (LCD_WIDTH - screen.w) / 2;
    int top = (LCD_HEIGHT - screen.h) / 2;
    rb->lcd_bitmap(scanout, left, top, screen.w, screen.h);
    rb->lcd_update();
    duke_service();
}

int SDL_FillRect(SDL_Surface *surface, SDL_Rect *rect, Uint32 color)
{
    if (surface != &screen || !screen.pixels)
        return -1;
    SDL_Rect area = rect ? *rect : screen.clip_rect;
    int x0 = MAX(0, area.x);
    int x1 = MIN(screen.w, area.x + area.w);
    for (int row = MAX(0, area.y); row < MIN(screen.h, area.y + area.h); row++)
        if (x1 > x0)
            rb->memset(pixels + row * screen.pitch + x0, color, x1 - x0);
    return 0;
}

int SDL_Flip(SDL_Surface *surface)
{
    SDL_UpdateRect(surface, 0, 0, 0, 0);
    return 0;
}

int SDL_LockSurface(SDL_Surface *surface)
{
    return surface == &screen ? 0 : -1;
}

void SDL_UnlockSurface(SDL_Surface *surface)
{
    (void)surface;
}

void SDL_WM_SetCaption(const char *title, const char *icon)
{
    (void)title;
    (void)icon;
}

SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode)
{
    if (mode != SDL_GRAB_QUERY)
        grab = mode;
    return grab;
}

int SDL_ShowCursor(int toggle)
{
    (void)toggle;
    return 0;
}

Uint8 SDL_GetMouseState(int *x, int *y)
{
    if (x)
        *x = 0;
    if (y)
        *y = 0;
    return 0;
}

void SDL_WarpMouse(Uint16 x, Uint16 y)
{
    (void)x;
    (void)y;
}

void SDL_SetModState(SDLMod mod)
{
    (void)mod;
}

/* MultiVoc mixing and reverb run on the same foreground stack. */
static char mutex_token;

SDL_mutex *SDL_CreateMutex(void)
{
    return (SDL_mutex *)&mutex_token;
}

int SDL_mutexP(SDL_mutex *mutex)
{
    (void)mutex;
    return 0;
}

int SDL_mutexV(SDL_mutex *mutex)
{
    (void)mutex;
    return 0;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
    (void)mutex;
}
