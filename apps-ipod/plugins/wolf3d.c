/* Wolf3D loader for the AstraPod SDK. GPL-2.0-or-later. */
#include "SDL.h"
#include "SDL_mixer.h"
#include "src/events/SDL_events_c.h"
#include "usb.h"

#define WOLF_HEAP_MIN (2u * 1024 * 1024)
#define WOLF_HEAP_MAX (3u * 1024 * 1024)
#define WOLF_DATA_DIR ROCKBOX_DIR "/wolf3d/"
#define WOLF_TAP_TICKS MAX(1, HZ / 50)
#define WOLF_EVENT_BUDGET 16

extern int my_main(int argc, char **argv);
extern int param_samplerate, param_audiobuffer;
static struct mutex heap_lock;
static unsigned int foreground;
static bool save_config;
static bool halted;

void *wolf_malloc(size_t bytes)
{
    if (bytes > WOLF_HEAP_MAX)
        return NULL;
    rb->mutex_lock(&heap_lock);
    void *ptr = tlsf_malloc(bytes);
    rb->mutex_unlock(&heap_lock);
    return ptr;
}

void *wolf_calloc(size_t count, size_t bytes)
{
    if (bytes && count > WOLF_HEAP_MAX / bytes)
        return NULL;
    rb->mutex_lock(&heap_lock);
    void *ptr = tlsf_calloc(count, bytes);
    rb->mutex_unlock(&heap_lock);
    return ptr;
}

void *wolf_realloc(void *ptr, size_t bytes)
{
    if (bytes > WOLF_HEAP_MAX)
        return NULL;
    rb->mutex_lock(&heap_lock);
    void *next = tlsf_realloc(ptr, bytes);
    rb->mutex_unlock(&heap_lock);
    return next;
}

void wolf_free(void *ptr)
{
    rb->mutex_lock(&heap_lock);
    tlsf_free(ptr);
    rb->mutex_unlock(&heap_lock);
}

static void drain(void *unused)
{
    (void)unused;
    if (halted)
        return;
    halted = true;
    Mix_CloseAudio();
    SDL_Quit();
    rb->rock_stream_close();
}

static void cleanup(void)
{
    drain(NULL);
}

void wolf_error(const char *message)
{
    drain(NULL);
    if (!rb->plugin_usb_taken())
        rb->splashf(3 * HZ, "%s", message);
    exit(1);
}

void wolf_allow_config_save(void)
{
    save_config = true;
}

bool wolf_can_save_config(void)
{
    return save_config && !halted;
}

enum { WOLF_ESCAPE_KEY = 5 };

static struct wolf_key {
    unsigned int buttons;
    SDLKey symbol;
    unsigned int held;
    unsigned long earliest_release;
    bool down;
} keys[] = {
#ifdef BUTTON_RC_UP
    {BUTTON_MENU | BUTTON_RC_UP, SDLK_UP},
    {BUTTON_PLAY | BUTTON_RC_DOWN, SDLK_DOWN},
    {BUTTON_LEFT | BUTTON_RC_LEFT, SDLK_LEFT},
    {BUTTON_RIGHT | BUTTON_RC_RIGHT, SDLK_RIGHT},
    {BUTTON_SELECT | BUTTON_RC_SELECT | BUTTON_RC_PLAY, SDLK_RETURN},
    [WOLF_ESCAPE_KEY] = {BUTTON_RC_MENU | BUTTON_RC_STOP, SDLK_ESCAPE},
#else
    {BUTTON_MENU, SDLK_UP},
    {BUTTON_PLAY, SDLK_DOWN},
    {BUTTON_LEFT, SDLK_LEFT},
    {BUTTON_RIGHT, SDLK_RIGHT},
    {BUTTON_SELECT, SDLK_RETURN},
    [WOLF_ESCAPE_KEY] = {0, SDLK_ESCAPE},
#endif
    {BUTTON_SCROLL_BACK, SDLK_a},
    {BUTTON_SCROLL_FWD, SDLK_d},
};

static void key_event(struct wolf_key *key, bool pressed)
{
    SDL_keysym sym = {.sym = key->symbol, .mod = KMOD_NONE};
    key->down = pressed;
    SDL_PrivateKeyboard(pressed ? SDL_PRESSED : SDL_RELEASED, &sym);
}

static void handle_key(unsigned int event, unsigned long now)
{
    const unsigned int wheel = BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD;
    unsigned int buttons = event & ~(BUTTON_REL | BUTTON_REPEAT);
    for (size_t i = 0; i < ARRAYLEN(keys); ++i)
    {
        struct wolf_key *key = &keys[i];
        unsigned int changed = key->buttons & buttons;
        if (!changed)
            continue;
        if (event & BUTTON_REL)
            key->held &= ~changed;
        else
        {
            key->held |= changed & ~wheel;
            key->earliest_release = now + WOLF_TAP_TICKS;
            if (!key->down)
                key_event(key, true);
        }
    }
}

void wolf_service(void)
{
    if (halted || rb->thread_self() != foreground)
        return;

    if (rb->plugin_usb_taken())
        exit(PLUGIN_USB_CONNECTED);

    wolf_audio_pump();
    bool video = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    unsigned long now = *rb->current_tick;
    for (unsigned int i = 0; i < WOLF_EVENT_BUDGET; ++i)
    {
        long event = rb->button_get(false);
        if (event == BUTTON_NONE)
            break;

        if (IS_SYSEVENT(event))
        {
            long result = rb->default_event_handler_ex(event, drain, NULL);
            if (result == SYS_USB_CONNECTED)
                exit(PLUGIN_USB_CONNECTED);
            if (result == SYS_POWEROFF || result == SYS_REBOOT)
                exit(PLUGIN_POWEROFF);
        }
        else if (video)
            handle_key(event, now);
    }

    if (!video)
        return;

#ifdef HAS_BUTTON_HOLD
    static bool hold;
    bool new_hold = rb->button_hold();
    if (new_hold != hold)
    {
        hold = new_hold;
        if (hold)
        {
            struct wolf_key *escape = &keys[WOLF_ESCAPE_KEY];
            escape->earliest_release = now + WOLF_TAP_TICKS;
            if (!escape->down)
                key_event(escape, true);
        }
    }
#endif

    for (size_t i = 0; i < ARRAYLEN(keys); ++i)
    {
        struct wolf_key *key = &keys[i];
        if (key->down && !key->held &&
            !TIME_BEFORE(now, key->earliest_release))
            key_event(key, false);
    }
}

static void check_data(void)
{
    static const char *const paths[] = {
        WOLF_DATA_DIR "vswap.wl1", WOLF_DATA_DIR "audiohed.wl1",
        WOLF_DATA_DIR "audiot.wl1", WOLF_DATA_DIR "gamemaps.wl1",
        WOLF_DATA_DIR "maphead.wl1", WOLF_DATA_DIR "vgadict.wl1",
        WOLF_DATA_DIR "vgagraph.wl1", WOLF_DATA_DIR "vgahead.wl1",
    };
    for (size_t i = 0; i < ARRAYLEN(paths); ++i)
    {
        wolf_service();
        if (!rb->file_exists(paths[i]))
        {
            char message[96];
            snprintf(message, sizeof(message), "Missing Wolf3D data: %s",
                     paths[i] + sizeof(WOLF_DATA_DIR) - 1);
            wolf_error(message);
        }
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    foreground = rb->thread_self();
    check_data();

    size_t granted;
    void *heap = rb->plugin_claim_scratch(WOLF_HEAP_MIN, WOLF_HEAP_MAX,
                                          &granted);
    if (!heap)
        wolf_error("Wolf3D needs 2 MiB of plugin scratch");
    rb->memset(heap, 0, sizeof(uint32_t));
    if (init_memory_pool(granted, heap) == (size_t)-1)
        wolf_error("Could not initialize the Wolf3D heap");
    rb->mutex_init(&heap_lock);

    if (atexit(cleanup))
        return PLUGIN_ERROR;
    rb->backlight_set_timeout(0);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    unsigned int rate = wolf_open_audio();
    if (rate)
        param_samplerate = rate;
    param_audiobuffer = 512;

    char *argv[] = {"wolf3d", NULL};
    return my_main(1, argv) ? PLUGIN_ERROR : PLUGIN_OK;
}
