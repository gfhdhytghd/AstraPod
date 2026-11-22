/* Cooperative, bounded playback of four-channel ProTracker modules. */
#include "plugins/rock_stream.h"

#define MOD_HEADER_SIZE 1084
#define MOD_PATTERN_SIZE 1024
#define MOD_MAX_BYTES (512u * 1024u)
#define MOD_MIN_ENGINE_BYTES (512u * 1024u)

static uint16_t big_endian_word(const unsigned char *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static bool valid_protracker(const unsigned char *data, size_t length)
{
    if (length < MOD_HEADER_SIZE ||
        rb->memcmp(data + 1080, "M.K.", 4) ||
        !data[950] || data[950] > 128)
        return false;

    unsigned int patterns = 1;
    for (unsigned int i = 0; i < 128; ++i)
    {
        /* The stock loader considers the whole order table for offsets. */
        if (data[952 + i] >= 128)
            return false;
        if (patterns <= data[952 + i])
            patterns = data[952 + i] + 1;
    }

    size_t end = MOD_HEADER_SIZE + patterns * MOD_PATTERN_SIZE;
    if (end > length)
        return false;

    for (unsigned int i = 0; i < 31; ++i)
    {
        const unsigned char *sample = data + 20 + 30 * i;
        unsigned int bytes = 2u * big_endian_word(sample + 22);
        unsigned int loop_start = 2u * big_endian_word(sample + 26);
        unsigned int loop_bytes = 2u * big_endian_word(sample + 28);
        if (sample[24] > 15 || sample[25] > 64 || bytes > length - end ||
            (loop_bytes > 2 &&
             (loop_start > bytes || loop_bytes > bytes - loop_start)) ||
            (bytes >= 5 && !rb->memcmp(data + end, "ADPCM", 5)))
            return false;
        end += bytes;
    }
    return end == length;
}

static void print_status(const MODULE *module, bool paused)
{
    unsigned int elapsed = module->sngtime >> 10;
    rb->lcd_clear_display();
    rb->lcd_puts(0, 0, "MikMod");
    rb->lcd_puts(0, 1, module->songname ? module->songname : "");
    rb->lcd_putsf(0, 3, "%s  %u:%02u", paused ? "Paused" : "Playing",
                  elapsed / 60, elapsed % 60);
    rb->lcd_putsf(0, 4, "Position %d / %u", module->sngpos + 1,
                  module->numpos);
    rb->lcd_puts(0, 6, "Play: pause or resume");
    rb->lcd_puts(0, 7, "Left / Right: change position");
    rb->lcd_puts(0, 8, "Menu: return");
    rb->lcd_update();
}

static enum plugin_status play_module(MODULE *module)
{
    static uint32_t block[ROCK_STREAM_BLOCK_BYTES / 4];
    bool paused = false;
    long next_draw = *rb->current_tick;
    Player_Start(module);
    print_status(module, paused);
    for (;;)
    {
        int pending = rb->rock_stream_pending();
        if (pending < 0 || (!Player_Active() && !pending))
            break;

        if (!paused && pending < ROCK_STREAM_QUEUE_SIZE && Player_Active())
        {
            size_t size = VC_WriteBytes((SBYTE *)block, sizeof(block));
            if (size > sizeof(block) ||
                (size && !rb->rock_stream_submit(block, size)))
                break;
        }
        if (TIME_AFTER(*rb->current_tick, next_draw))
        {
            print_status(module, paused);
            next_draw = *rb->current_tick + HZ / 4;
        }
        rb->reset_poweroff_timer();
        int action = rb->get_action(CONTEXT_WPS,
                                    paused || pending ? 1 : TIMEOUT_NOBLOCK);
        switch (action)
        {
            case ACTION_WPS_MENU:
            case ACTION_WPS_STOP:
            case ACTION_STD_CANCEL:
                return PLUGIN_OK;
            case ACTION_WPS_PLAY:
                paused = !paused;
                if (!rb->rock_stream_pause(paused))
                    return PLUGIN_OK;
                print_status(module, paused);
                break;
            case ACTION_WPS_SKIPPREV:
            case ACTION_WPS_SKIPNEXT:
                rb->rock_stream_close();
                if (!rb->rock_stream_open() ||
                    !rb->rock_stream_pause(paused))
                    return PLUGIN_OK;
                if (action == ACTION_WPS_SKIPPREV)
                    Player_PrevPosition();
                else
                    Player_NextPosition();
                print_status(module, paused);
                break;
            case ACTION_WPS_VOLUP:
                rb->adjust_volume(1);
                break;
            case ACTION_WPS_VOLDOWN:
                rb->adjust_volume(-1);
                break;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;
                break;
        }
    }
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    char choice[MAX_PATH];
    const char *path = parameter;
    if (!path)
    {
        if (rb->pick_file(choice, sizeof(choice), "/", "Choose ProTracker MOD",
                          ".mod", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = choice;
    }

    size_t capacity;
    unsigned char *memory = rb->plugin_claim_scratch(
        MOD_MAX_BYTES + MOD_MIN_ENGINE_BYTES, SIZE_MAX, &capacity);
    if (!memory)
        return PLUGIN_ERROR;

    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return PLUGIN_ERROR;
    off_t size = rb->ffilesize(fd);
    bool loaded = size >= MOD_HEADER_SIZE && size <= MOD_MAX_BYTES &&
                  rb->read(fd, memory, size) == size;
    if (rb->close(fd) < 0 || !loaded || !valid_protracker(memory, size))
    {
        rb->splash(2 * HZ, "Unsupported or incomplete ProTracker MOD");
        return PLUGIN_ERROR;
    }

    unsigned int rate = rb->rock_stream_open();
    if (!rate)
    {
        rb->splash(2 * HZ, "Stop playback before opening MikMod");
        return PLUGIN_OK;
    }

    void *arena = memory + MOD_MAX_BYTES;
    if (init_memory_pool(capacity - MOD_MAX_BYTES, arena) == (size_t)-1)
    {
        rb->rock_stream_close();
        return PLUGIN_ERROR;
    }

    MikMod_RegisterDriver(&drv_nos);
    MikMod_RegisterLoader(&load_mod);
    md_mode = DMODE_STEREO | DMODE_16BITS | DMODE_SOFT_MUSIC | DMODE_SOFT_SNDFX;
    md_mixfreq = rate;
    enum plugin_status result = PLUGIN_ERROR;
    if (!MikMod_Init(""))
    {
        MODULE *module = Player_LoadMem((const char *)memory, size, 4, 0);
        if (module)
        {
            module->wrap = false;
            module->loop = false;
            result = play_module(module);
            Player_Stop();
            Player_Free(module);
        }
        MikMod_Exit();
    }
    destroy_memory_pool(arena);
    rb->rock_stream_close();
    return result;
}
