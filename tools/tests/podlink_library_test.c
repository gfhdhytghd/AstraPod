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
unsigned int art_cache_track_key(const char *path) { (void)path; return 0; }
int art_cache_size_dim(int size) { (void)size; return 0; }
int art_cache_size_index(const char *name) { (void)name; return -1; }
bool art_cache_lookup_hash(unsigned int key, int size, char *path, int capacity,
                           bool *fallback)
{
    (void)key; (void)size; (void)path; (void)capacity; (void)fallback;
    return false;
}
bool art_folder_file(const char *path, char *out, int size)
{
    (void)path; (void)out; (void)size;
    return false;
}

static unsigned int limit = 384;
unsigned int podlink_max_message(void) { return limit; }

static char reply[MEDIA_REPLY_SIZE];
static unsigned char chunk[PODLINK_MAX_MESSAGE];

static void command(const char *text)
{
    struct podlink_message message = {
        .type = PODLINK_TEXT,
        .length = strlen(text),
        .data = (const unsigned char *)text,
    };
    assert(podlink_media_enqueue(&message));
    podlink_media_work();
    reply[podlink_media_next(reply, sizeof(reply) - 1)] = '\0';
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
        assert(podlink_media_next_binary(chunk, 8) == 0);
        unsigned int n = podlink_media_next_binary(chunk, limit);
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
    assert(podlink_media_next_binary(chunk, sizeof(chunk)) == 0);

    command("MS3 FILE " TAGCACHE_FILE_MASTER);
    command("MS3 NEXT");
    assert(!reply[0]);
    podlink_media_disconnect();
    assert(podlink_media_next_binary(chunk, sizeof(chunk)) == 0);
    podlink_media_work();
    command("MS3 NEXT");
    assert(!strcmp(reply, "MS3 ERR state"));
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
