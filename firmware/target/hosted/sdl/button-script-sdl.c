/* Simulator-only button script and LCD capture. GPL version 2+.
 *
 * --keys FILE (or PODBOX_UI_KEYS) feeds the regular SDL event loop. Each
 * line is `down NAME`, `up NAME`, or `wait N` at 60 f. NAME is up, down,
 * back, right, select, menu, play, hold or usb (ipod6g). `-` reads stdin.
 *
 * PODBOX_UI_WALK_STATE maps a shared RGB24 ring of recent LCD frames.
 * PODBOX_UI_WALK_RENDER_DELAY=events:frames holds one present after that
 * many accepted key events so a queued release can race a busy LCD. */

#include "config.h"

#ifdef SIMULATOR

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <unistd.h>

#include "SDL.h"

#include "button-script-sdl.h"

#define WIDTH 320
#define HEIGHT 240
#define FRAME_COUNT 8
#define FRAME_BYTES (WIDTH * HEIGHT * 3)

struct frame {
    _Atomic uint32_t sequence;
    unsigned char pixels[FRAME_BYTES];
};

struct shared_state {
    _Atomic uint32_t presents;
    _Atomic uint32_t captured;
    _Atomic uint32_t accepted;
    struct frame frames[FRAME_COUNT];
};

static const struct {
    const char *name;
    SDL_Keycode key;
} keys[] = {
    { "up", SDLK_UP },
    { "down", SDLK_DOWN },
    { "back", SDLK_LEFT },
    { "right", SDLK_RIGHT },
    { "select", SDLK_RETURN },
    { "menu", SDLK_ESCAPE },
    { "play", SDLK_SPACE },
    { "hold", SDLK_h },
#ifdef ASTRAPOD_ROCK_COMPAT
    { "usb", SDLK_u },
#endif
};

static struct shared_state *shared;
static FILE *script;
static const char *keys_path;
static unsigned delay_after_events;
static unsigned delay_frames;

static void die(const char *message)
{
    fprintf(stderr, "*PANIC* button script: %s\n", message);
    _exit(125);
}

static SDL_Keycode lookup_key(const char *name)
{
    for (unsigned i = 0; i < (unsigned)(sizeof(keys) / sizeof(keys[0])); i++)
        if (!strcmp(keys[i].name, name))
            return keys[i].key;
    return SDLK_UNKNOWN;
}

static void push_key(SDL_Keycode key, int pressed)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    if (SDL_PushEvent(&event) != 1)
        die(SDL_GetError());
    if (shared)
        atomic_fetch_add_explicit(&shared->accepted, 1, memory_order_relaxed);
}

static void wait_presents(unsigned frames)
{
    if (!frames)
        return;
    if (!shared)
    {
        SDL_Delay((frames * 1000 + 59) / 60);
        return;
    }
    uint32_t start = atomic_load_explicit(&shared->presents, memory_order_relaxed);
    while (atomic_load_explicit(&shared->presents, memory_order_relaxed) - start < frames)
        SDL_Delay(2);
}

static int script_thread(void *unused)
{
    char line[128];
    (void)unused;
    while (fgets(line, sizeof(line), script))
    {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char cmd[16], name[16];
        unsigned frames = 0;
        if (sscanf(line, "%15s %15s", cmd, name) == 2 &&
            (!strcmp(cmd, "down") || !strcmp(cmd, "up")))
        {
            SDL_Keycode key = lookup_key(name);
            if (key == SDLK_UNKNOWN)
                die(line);
            push_key(key, cmd[0] == 'd');
        }
        else if (sscanf(line, "wait %u", &frames) == 1)
            wait_presents(frames);
        else
            die(line);
    }
    return 0;
}

void button_script_sdl_set_path(const char *path)
{
    keys_path = path;
}

static struct shared_state *map_walk_state(const char *path)
{
#ifdef WIN32
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        die("cannot open walk state");
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 0, NULL);
    CloseHandle(file);
    if (!mapping)
        return NULL;
    void *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(*shared));
    CloseHandle(mapping);
    return view;
#else
    int fd = open(path, O_RDWR);
    if (fd < 0)
        die("cannot open walk state");
    void *view = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    close(fd);
    return view == MAP_FAILED ? NULL : view;
#endif
}

void button_script_sdl_start(void)
{
    const char *path = keys_path ? keys_path : getenv("PODBOX_UI_KEYS");
    const char *state_path = getenv("PODBOX_UI_WALK_STATE");
    const char *delay = getenv("PODBOX_UI_WALK_RENDER_DELAY");

    if (state_path)
    {
        shared = map_walk_state(state_path);
        if (!shared)
            die("cannot map walk state");
    }
    if (delay && sscanf(delay, "%u:%u", &delay_after_events, &delay_frames) != 2)
        die("invalid render delay");
    if (!path)
        return;

    script = !strcmp(path, "-") ? stdin : fopen(path, "r");
    if (!script)
        die("cannot open key script");
    setvbuf(script, NULL, _IOLBF, 0);

    SDL_Thread *thread = SDL_CreateThread(script_thread, "keys", NULL);
    if (!thread)
        die("cannot start key script thread");
    SDL_DetachThread(thread);
}

void button_script_sdl_present(const void *rgb565, int pitch)
{
    if (!shared || !rgb565 || pitch < WIDTH * 2)
        return;

    uint32_t captured = atomic_load_explicit(&shared->captured, memory_order_relaxed);
    struct frame *frame = &shared->frames[captured % FRAME_COUNT];
    atomic_store_explicit(&frame->sequence, 0, memory_order_relaxed);

    unsigned char *dst = frame->pixels;
    for (int y = 0; y < HEIGHT; y++)
    {
        const uint16_t *row = (const uint16_t *)((const unsigned char *)rgb565 + y * pitch);
        for (int x = 0; x < WIDTH; x++)
        {
            uint16_t pixel = row[x];
            *dst++ = (unsigned char)(((pixel >> 11) * 255 + 15) / 31);
            *dst++ = (unsigned char)((((pixel >> 5) & 63) * 255 + 31) / 63);
            *dst++ = (unsigned char)(((pixel & 31) * 255 + 15) / 31);
        }
    }

    atomic_store_explicit(&frame->sequence, captured + 1, memory_order_release);
    atomic_store_explicit(&shared->captured, captured + 1, memory_order_release);
    if (delay_frames &&
        atomic_load_explicit(&shared->accepted, memory_order_relaxed) >= delay_after_events)
    {
        SDL_Delay((delay_frames * 1000 + 59) / 60);
        delay_frames = 0;
    }
    atomic_fetch_add_explicit(&shared->presents, 1, memory_order_relaxed);
}

#endif /* SIMULATOR */
