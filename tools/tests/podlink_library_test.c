/* Exercise MS3 against a real tagcache, with the DBTOOL filesystem and kernel
 * stubs. Build with make -C tools/database_pb test-library NATDIR=<5G sim build>.
 * Pass a disposable player root containing .rockbox/library/database.
 * Stdout contains the unmodified DB/NEXT walk replies for host parity tests.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "dir.h"
#include "mv.h"
#include "pathfuncs.h"
#include "crc32.h"
#include "database/tagcache.h"
#include "metadata/art_cache.h"
#include "metadata/albumart.h"
#include "screens/system/podlink_media.h"
#include "system/bg_task.h"
#include "system/library_files.h"

const char *sim_root_dir;
const char rbversion[] = "host test";
int database_pb_verbose;
bool database_pb_rebuild;
struct bg_task tagcache_task;

void bg_task_update(struct bg_task *task) { (void)task; assert(0); }
void volume_size(IF_MV(int volume,) sector_t *size, sector_t *free)
{
    IF_MV((void)volume;)
    *size = *free = 0;
}
#define TEST_ART_PATH "/test-ms3-art.aat"

unsigned int art_cache_track_key(const char *path)
{
    return !strcmp(path, "/Music/track with spaces.flac");
}
int art_cache_size_dim(int size) { return size == 0 ? 44 : 0; }
int art_cache_size_index(const char *name) { return strcmp(name, "list") ? -1 : 0; }
bool art_cache_lookup_hash(unsigned int key, int size, char *path, int capacity,
                           bool *fallback)
{
    *fallback = false;
    if (key != 1 || size != 0)
        return false;
    snprintf(path, capacity, "%s", TEST_ART_PATH);
    return true;
}
bool art_folder_file(const char *path, char *out, int size)
{
    (void)path; (void)out; (void)size;
    return false;
}

static unsigned int limit = 384;
unsigned int podlink_max_message(void) { return limit; }
void podlink_wake(void) {}
uint32_t podlink_bulk_crc(const unsigned char *data, unsigned int length,
                          uint32_t previous, uint32_t *raw)
{
    *raw = crc_32r(data, length, 0);
    return crc_32r(data, length, previous);
}

static char reply[MEDIA_REPLY_SIZE];
static unsigned char chunk[PODLINK_MAX_MESSAGE];

static void enqueue(const char *text)
{
    struct podlink_message message = {
        .type = PODLINK_TEXT,
        .length = strlen(text),
        .data = (const unsigned char *)text,
    };
    assert(podlink_media_enqueue(&message));
}

static bool read_text(void)
{
    bool refill = false;
    reply[podlink_media_next(reply, sizeof(reply) - 1, &refill)] = '\0';
    return refill;
}

static void command(const char *text)
{
    enqueue(text);
    podlink_media_work();
    read_text();
}

static unsigned int read_binary(unsigned int capacity)
{
    bool refill = false;
    uint32_t raw;
    return podlink_media_next_binary(chunk, capacity, &refill, &raw);
}

static void check_file(void)
{
    int fd = open(LIB_DB_DIR "/" TAGCACHE_FILE_MASTER, O_RDONLY);
    assert(fd >= 0);
    unsigned int length = ffilesize(fd);
    assert(length > limit - 8);

    command("MS3 FILE " TAGCACHE_FILE_MASTER);
    char expected[MEDIA_REPLY_SIZE];
    snprintf(expected, sizeof(expected), "MS3 FILE %u", length);
    assert(!strcmp(reply, expected));
    command("MS1 REFRESH");
    assert(!strcmp(reply, "MS1 ERR busy"));

    unsigned int offset = 0;
    uint32_t crc = UINT32_MAX;
    while (offset < length)
    {
        command("MS3 NEXT");
        assert(!reply[0]);
        assert(read_binary(8) == 0);
        unsigned int n = read_binary(limit);
        assert(n == MIN(limit - 8, length - offset) + 8);
        assert(!memcmp(chunk, "MSF1", 4));
        uint32_t position = 0;
        for (unsigned int i = 0; i < 4; i++)
            position |= (uint32_t)chunk[i + 4] << (8 * i);
        assert(position == offset);
        unsigned char source[PODLINK_MAX_MESSAGE];
        assert(read(fd, source, n - 8) == (ssize_t)n - 8);
        assert(!memcmp(source, chunk + 8, n - 8));
        crc = crc_32r(source, n - 8, crc);
        offset += n - 8;
    }
    close(fd);
    command("MS3 NEXT");
    snprintf(expected, sizeof(expected), "MS3 DONE %u %08lx", length,
             (unsigned long)(crc ^ UINT32_MAX));
    assert(!strcmp(reply, expected));

    command("MS3 FILE " TAGCACHE_FILE_MASTER);
    tagcache_get_stat()->scan_removed++;
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR database changed"));
    assert(read_binary(sizeof(chunk)) == 0);

    command("MS3 FILE " TAGCACHE_FILE_MASTER);
    command("MS3 NEXT");
    assert(!reply[0]);
    podlink_media_disconnect();
    assert(read_binary(sizeof(chunk)) == 0);
    podlink_media_work();
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR state"));
}

static void check_artwork(void)
{
    const char *path = "list /Music/track with spaces.flac";
    struct art_cache_header header = {
        .magic = ART_CACHE_MAGIC,
        .version = ART_CACHE_FORMAT_VERSION,
        .width = 20,
        .height = 20,
        .layout = AA_COLUMNS,
    };
    unsigned char pixels[20 * 20 * 2];
    for (unsigned int i = 0; i < sizeof(pixels); i++)
        pixels[i] = i;
    int fd = open(TEST_ART_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, &header, sizeof(header)) == sizeof(header));
    assert(write(fd, pixels, sizeof(pixels)) == sizeof(pixels));
    close(fd);

    char request[MEDIA_REPLY_SIZE];
    snprintf(request, sizeof(request), "MS3 ARTBIN %s", path);
    command(request);
    assert(!strcmp(reply, "MS3 ARTBIN 20 20 columns"));
    command("MS3 STOP");
    assert(!strcmp(reply, "MS3 STOPPED"));
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR state"));

    command(request);
    assert(!strcmp(reply, "MS3 ARTBIN 20 20 columns"));
    for (unsigned int offset = 0; offset < sizeof(pixels);)
    {
        command("MS3 NEXT");
        assert(!reply[0]);
        unsigned int length = MIN(limit - 8, sizeof(pixels) - offset);
        assert(read_binary(limit) == length + 8);
        assert(!memcmp(chunk, "MSF1", 4));
        for (unsigned int i = 0; i < 4; i++)
            assert(chunk[i + 4] == ((offset >> (8 * i)) & 0xff));
        assert(!memcmp(chunk + 8, pixels + offset, length));
        offset += length;
    }
    command("MS3 NEXT");
    char expected[MEDIA_REPLY_SIZE];
    snprintf(expected, sizeof(expected), "MS3 DONE %zu %08lx", sizeof(pixels),
             (unsigned long)(crc_32r(pixels, sizeof(pixels), UINT32_MAX) ^
                             UINT32_MAX));
    assert(!strcmp(reply, expected));

    /* Force precisely eight chunks. DONE requires the next credit window. */
    limit = 108;
    snprintf(request, sizeof(request), "MS3 STREAM ARTBIN %s", path);
    enqueue(request);
    podlink_media_work();
    /* An unrelated disk wake must preserve unconsumed metadata and data. */
    podlink_media_work();
    assert(!read_binary(limit));
    assert(read_text());
    assert(!strcmp(reply, "MS3 ARTBIN 20 20 columns"));
    for (unsigned int i = 0; i < 8; i++)
    {
        bool refill = false;
        podlink_media_work();
        uint32_t raw;
        assert(podlink_media_next_binary(chunk, limit, &refill, &raw) == limit);
        assert(raw == crc_32r(chunk + 8, limit - 8, 0));
        assert(refill == (i < 7));
        assert(chunk[4] == ((i * 100) & 0xff));
        assert(!memcmp(chunk + 8, pixels + i * 100, 100));
    }
    podlink_media_work();
    assert(!read_text() && !reply[0]);
    assert(!read_binary(limit));
    command("MS3 CREDIT");
    assert(!strcmp(reply, expected));

    command(request);
    podlink_media_work(); /* One binary reply staged but unread. */
    command("MS3 STOP");
    assert(!strcmp(reply, "MS3 STOPPED"));
    assert(!read_binary(limit));
    command("MS3 CREDIT");
    assert(!strcmp(reply, "MS3 ERR state"));

    limit = 384;
    snprintf(request, sizeof(request), "MS3 ART %s", path);
    command(request);
    assert(!strcmp(reply, "MS3 ART 20 20 columns"));
    command("MS3 NEXT");
    assert(!strncmp(reply, "MS3 DATA + ", 11));
    command("MS3 NEXT");
    assert(!strncmp(reply, "MS3 DATA + ", 11));
    command("MS3 NEXT");
    assert(!strncmp(reply, "MS3 DATA . ", 11));
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR state"));
    assert(remove(TEST_ART_PATH) == 0);
}

static void check_sample(void)
{
    const char *path = "/iPod_Control/Music/F00/profile sample.flac";
    mkdir("/iPod_Control");
    mkdir("/iPod_Control/Music");
    mkdir("/iPod_Control/Music/F00");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    const char data[] = "profiling fixture";
    assert(write(fd, data, sizeof(data)) == sizeof(data));
    close(fd);

    command("MS3 STREAM SAMPLE /outside.flac");
    assert(!strcmp(reply, "MS3 ERR sample path"));
    command("MS3 STREAM SAMPLE /Music/PodLink/../outside.flac");
    assert(!strcmp(reply, "MS3 ERR sample path"));
    command("MS3 STREAM SAMPLE /iPod_Control/Music/.. /outside.flac");
    assert(!strcmp(reply, "MS3 ERR sample path"));
    command("MS3 STREAM SAMPLE /iPod_Control/Music/F00/profile sample.flac");
    char expected[MEDIA_REPLY_SIZE];
    snprintf(expected, sizeof(expected), "MS3 SAMPLE %zu", sizeof(data));
    assert(!strcmp(reply, expected));
    podlink_media_work();
    assert(read_binary(limit) == sizeof(data) + 8);
    assert(!memcmp(chunk, "MSF1\0\0\0\0", 8));
    assert(!memcmp(chunk + 8, data, sizeof(data)));
    podlink_media_work();
    read_text();
    snprintf(expected, sizeof(expected), "MS3 DONE %zu %08lx", sizeof(data),
             (unsigned long)(crc_32r(data, sizeof(data), UINT32_MAX) ^
                             UINT32_MAX));
    assert(!strcmp(reply, expected));
    assert(remove(path) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    sim_root_dir = argv[1];
    init_volume_names();
    assert(tagcache_tool_open());
    podlink_media_init();

    command("MS3 FILES");
    assert(!strncmp(reply, "MS3 FILES ", 10));
    assert(strstr(reply, "\tdatabase_idx.tcd:"));
    assert(strstr(reply, "\tdatabase_12.tcd:"));
    assert(!strstr(reply, "database_9.tcd:"));

    const char *denied[] = {"MS3 FILE ../database_idx.tcd",
                            "MS3 FILE database_idx.tcd.new",
                            "MS3 FILE database_tmp.tcd", "MS3 FILE database_9.tcd"};
    for (unsigned int i = 0; i < sizeof(denied) / sizeof(*denied); i++)
    {
        command(denied[i]);
        assert(!strcmp(reply, "MS3 ERR database file"));
    }

    tagcache_get_stat()->scanning = true;
    command("MS3 FILES");
    assert(!strcmp(reply, "MS3 ERR database unavailable"));
    tagcache_get_stat()->scanning = false;
    check_file();
    check_artwork();
    check_sample();

    command("MS3 DB");
    assert(!strcmp(reply, "MS3 READY"));
    do
    {
        command("MS3 NEXT");
        assert(!strncmp(reply, "MS3 DATA ", 9) ||
               !strcmp(reply, "MS3 MORE") || !strcmp(reply, "MS3 END"));
        puts(reply);
    } while (strcmp(reply, "MS3 END"));

    command("MS3 FILE " TAGCACHE_FILE_MASTER);
    tagcache_update_numeric(0, tag_playcount, 10);
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR database changed"));
    command("MS3 FILES");
    assert(!strcmp(reply, "MS3 ERR database unavailable"));
    return 0;
}
