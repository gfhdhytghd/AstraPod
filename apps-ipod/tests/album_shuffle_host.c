/* Host checks for album grouping and the order journal. GPL version 2+.
 * See album_shuffle_host.sh for the build command.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "playlist/album_shuffle.h"

static const struct {
    const char *path, *album, *artist;
    int disc, track;
} fixture[] = {
    { "/set/Disc 2/A.mp3", "Collection", "Various Artists", 2, 1 },
    { "/band/B.mp3", "Collection", "One Band", 1, 2 },
    { "/loose/10.mp3", NULL, NULL, 0, 0 },
    { "/set/Disc 1/Z.mp3", "collection", "various artists", 1, 2 },
    { "/mix/second.mp3", "Mix", NULL, 0, 2 },
    { "/band/C.mp3", "Collection", "One Band", 1, 1 },
    { "/loose/2.mp3", NULL, NULL, 0, 0 },
    { "/set/Disc 1/B.mp3", "Collection", "Various Artists", 1, 1 },
    { "/mix/first.mp3", "Mix", NULL, 0, 1 },
    { "/other/track.mp3", NULL, NULL, 0, 0 },
};

#define COUNT (sizeof(fixture) / sizeof(fixture[0]))

static void filename(void *data, int index, char *path, int size)
{
    (void)data;
    snprintf(path, size, "%s", fixture[index].path);
}

static void tags(void *data, const char *path, const char **album,
                 const char **artist, int *disc, int *track)
{
    (void)data;
    for (size_t i = 0; i < COUNT; i++)
    {
        if (!strcmp(path, fixture[i].path))
        {
            *album = fixture[i].album;
            *artist = fixture[i].artist;
            *disc = fixture[i].disc;
            *track = fixture[i].track;
            return;
        }
    }
    assert(!"unknown fixture track");
}

static void assert_group(const uint16_t *order, const uint16_t *expected,
                         size_t size)
{
    for (size_t i = 0; i + size <= COUNT; i++)
        if (!memcmp(order + i, expected, size * sizeof(*expected)))
            return;
    assert(!"missing or misordered album");
}

static void check_groups(void)
{
    const struct album_shuffle_source source = { NULL, filename, tags };
    struct album_shuffle_track tracks[COUNT];
    uint16_t groups[COUNT], order[COUNT], first[COUNT];
    const uint16_t multi[] = { 7, 3, 0 }, band[] = { 5, 1 };
    const uint16_t loose[] = { 6, 2 }, mix[] = { 8, 4 };
    bool moved = false;
    album_shuffle_read(&source, COUNT, tracks);
    for (unsigned seed = 1; seed < 30; seed++)
    {
        album_shuffle_order(tracks, COUNT, -1, seed, groups, order);
        assert_group(order, multi, 3);
        assert_group(order, band, 2);
        assert_group(order, loose, 2);
        assert_group(order, mix, 2);
        const uint16_t other[] = { 9 };
        assert_group(order, other, 1);
        if (seed == 1)
            memcpy(first, order, sizeof(first));
        else if (memcmp(first, order, sizeof(first)))
            moved = true;
    }
    assert(moved);

    album_shuffle_order(tracks, COUNT, 3, 25, groups, order);
    assert(order[0] == 3 && order[1] == 7 && order[2] == 0);
    uint16_t pinned[COUNT];
    memcpy(pinned, order, sizeof(pinned));
    album_shuffle_reindex(tracks, COUNT, groups, order);
    album_shuffle_order(tracks, COUNT, -1, 1, groups, order);
    for (size_t i = 0; i < COUNT; i++)
        assert(first[i] == pinned[order[i]]);
}

static void check_journal(void)
{
    enum { SIZE = 130 };
    uint16_t order[SIZE], loaded[SIZE];
    for (int i = 0; i < SIZE; i++)
        order[i] = SIZE - 1 - i;
    FILE *file = tmpfile();
    assert(file);
    int fd = fileno(file);
    assert(write(fd, "P:6::\n", 6) == 6);
    off_t offset = lseek(fd, 0, SEEK_CUR);
    assert(album_shuffle_save(fd, order, SIZE, 5678));
    off_t end = lseek(fd, 0, SEEK_CUR);
    const char command[] = "B:5678:6\n";
    off_t record = end - (sizeof(command) - 1);
    assert(album_shuffle_load(fd, offset, record, loaded, SIZE));
    assert(lseek(fd, 0, SEEK_CUR) == end);
    assert(!memcmp(order, loaded, sizeof(order)));
    assert(!album_shuffle_load(fd, offset, record - 1, loaded, SIZE));

    /* Duplicate or out-of-range source positions invalidate the record. */
    assert(pwrite(fd, "0080", 4, offset + 1) == 4);
    assert(!album_shuffle_load(fd, offset, record, loaded, SIZE));
    assert(pwrite(fd, "ffff", 4, offset + 1) == 4);
    assert(!album_shuffle_load(fd, offset, record, loaded, SIZE));
    assert(lseek(fd, 0, SEEK_CUR) == end);
    fclose(file);
}

int main(void)
{
    check_groups();
    check_journal();
    puts("Album grouping, play-current order and control records pass");
    return 0;
}
