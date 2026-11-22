/* Run the XRick resource parser and foreground WAV producer on a host. GPL v2+. */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "plugin.h"
#include "xrick-compat.h"
#include "xrick/maps.h"
#include "xrick/resources.h"

static _Alignas(max_align_t) unsigned char arena[256 * 1024];
static int files, blocks;
static unsigned sink_rate;
static bool stream, paused, revoked, errors;
static int16_t first_sample;
static size_t samples;

static void *claim(size_t minimum, size_t preferred, size_t *granted)
{
    assert(minimum <= sizeof(arena) && preferred == sizeof(arena));
    *granted = sizeof(arena);
    memset(arena, 0xdd, sizeof(arena));
    return arena;
}

static int tracked_open(const char *path, int flags, ...)
{
    int fd = open(path, flags);
    files += fd >= 0;
    return fd;
}

static int tracked_close(int fd)
{
    assert(files > 0);
    files--;
    return close(fd);
}

static off_t file_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) || !S_ISREG(st.st_mode) ? -1 : st.st_size;
}

static unsigned stream_open(void)
{
    stream = true;
    return sink_rate;
}

static int pending(void)
{
    assert(stream);
    return revoked ? -1 : blocks;
}

static bool submit(const void *data, size_t size)
{
    assert(stream && !paused && ++blocks <= ROCK_STREAM_QUEUE_SIZE);
    assert(size && size % 4 == 0 && size <= ROCK_STREAM_BLOCK_BYTES);
    const int16_t *pcm = data;
    if (!samples)
        first_sample = pcm[0];
    for (size_t i = 0; i < size / 2; i += 2)
        assert(pcm[i] == pcm[i + 1]);
    samples += size / 4;
    return true;
}

static bool stream_pause(bool pause)
{
    assert(stream);
    paused = pause;
    return !revoked;
}

static void stream_close(void)
{
    assert(stream);
    stream = false;
    blocks = 0;
}

static const struct plugin_api api = {
    .plugin_claim_scratch = claim,
    .memset = memset, .strlen = strlen, .snprintf = snprintf,
    .open = tracked_open, .read = read, .close = tracked_close,
    .lseek = lseek, .ffilesize = file_size,
    .rock_stream_open = stream_open, .rock_stream_pending = pending,
    .rock_stream_submit = submit, .rock_stream_pause = stream_pause,
    .rock_stream_close = stream_close,
};
const struct plugin_api *rb = &api;

bool xrick_load_interrupted(void)
{
    return false;
}

size_t sys_strlen(const char *text) { return strlen(text); }

/* The resource probe checks the table's bounds without dispatching entities. */
#define UNUSED_ENTITY(name) void name(U8 type) { (void)type; abort(); }
UNUSED_ENTITY(e_rick_action)
UNUSED_ENTITY(e_bullet_action)
UNUSED_ENTITY(e_bomb_action)
UNUSED_ENTITY(e_them_t1a_action)
UNUSED_ENTITY(e_them_t1b_action)
UNUSED_ENTITY(e_them_t2_action)
UNUSED_ENTITY(e_box_action)
UNUSED_ENTITY(e_bonus_action)
UNUSED_ENTITY(e_sbonus_start)
UNUSED_ENTITY(e_sbonus_stop)
#undef UNUSED_ENTITY

void sys_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    errors = true;
}

static void audio_probe(unsigned rate)
{
    sink_rate = rate;
    assert(syssnd_init());
    syssnd_play(soundBombshht, 1);
    assert(files == 1);
    syssnd_update();
    assert(samples && first_sample == -512);
    syssnd_pauseAll(true);
    size_t before = samples;
    syssnd_update();
    assert(samples == before);
    syssnd_pauseAll(false);
    blocks = 0;
    syssnd_update();
    assert(!files);
    assert(samples == (soundBombshht->len * rate + Wave_SAMPLE_RATE - 1) /
                       Wave_SAMPLE_RATE);

    syssnd_play(soundBombshht, -1);
    blocks = 0;
    syssnd_update();
    assert(files == 1 && blocks == ROCK_STREAM_QUEUE_SIZE);
    revoked = true;
    syssnd_update();
    assert(!files && !stream && !blocks);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    bool valid = !strcmp(argv[2], "valid");
    close(STDIN_FILENO); /* fd zero must work as a distinct file_t. */
    assert(sysmem_init());
    assert(sysmem_push(SIZE_MAX) == NULL);
    assert(sysfile_setRootPath(argv[1]));
    assert(resources_load() == valid);
    assert(files == 0);
    if (valid)
    {
        assert(!map_window_valid(map_nbr_submaps, 0));
        audio_probe(44100);
        revoked = false;
        samples = 0;
        audio_probe(48000);
    }
    errors = false;
    resources_unload();
    sysfile_clearRootPath();
    sysmem_shutdown();
    assert(!errors && !files);
    return 0;
}
