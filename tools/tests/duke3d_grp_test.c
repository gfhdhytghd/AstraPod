/* Focused host probe for Duke3D GRP bounds. GNU GPL version 2 or later. */
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXGROUPFILES 4
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int32_t groupefil_crc32[MAXGROUPFILES];
static unsigned service_calls;
static unsigned files;
static int short_read_at = -1;

static int duke_open(const char *name, int flags)
{
    int fd = open(name, flags);
    if (fd >= 0)
        files++;
    return fd;
}

static int test_close(int fd)
{
    assert(files);
    files--;
    return close(fd);
}

static off_t test_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_size : -1;
}

static ssize_t test_read(int fd, void *buffer, size_t count)
{
    if (short_read_at >= 0 && short_read_at-- == 0)
        return 0;
    return read(fd, buffer, count);
}

static const struct {
    off_t (*ffilesize)(int);
} api = {test_size};
static const typeof(api) *rb = &api;

static void duke_service(void)
{
    service_calls++;
}

static uint32_t crc32_update(uint8_t *bytes, int length, uint32_t crc)
{
    for (int i = 0; i < length; i++)
        crc += bytes[i];
    return crc;
}

#define read test_read
#define close test_close
#include "../../apps-ipod/plugins/duke3d/grp.inc"
#undef close
#undef read

static void replace_archive(const char *path, const void *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    assert(write(fd, data, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

int main(void)
{
    char path[] = "/tmp/duke-grp-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    uint8_t grp[16 + 32 + 5] = "KenSilverman";
    grp[12] = 2;
    memcpy(grp + 16, "ONE", 3);
    grp[28] = 3;
    memcpy(grp + 32, "TWO", 3);
    grp[44] = 2;
    memcpy(grp + 48, "abcde", 5);

    for (size_t n = 0; n < sizeof(grp); n++)
    {
        replace_archive(path, grp, n);
        assert(initgroupfile(path) == -1 && files == 0);
    }
    grp[15] = 0xff;
    replace_archive(path, grp, sizeof(grp));
    assert(initgroupfile(path) == -1 && files == 0);
    grp[15] = 0;
    grp[31] = 0xff;
    replace_archive(path, grp, sizeof(grp));
    assert(initgroupfile(path) == -1 && files == 0);
    grp[31] = 0;
    replace_archive(path, grp, sizeof(grp));

    fd = open(path, O_WRONLY);
    assert(fd >= 0);
    assert(ftruncate(fd, DUKE_GRP_MAX_BYTES + 1) == 0);
    assert(close(fd) == 0);
    assert(initgroupfile(path) == -1 && files == 0);
    replace_archive(path, grp, sizeof(grp));

    for (int step = 0; step < 4; step++)
    {
        short_read_at = step;
        assert(initgroupfile(path) == -1 && files == 0);
    }

    service_calls = 0;
    assert(initgroupfile(path) == 0 && files == 1 && service_calls == 2);

    int one = kopen4load("ONE", 1);
    int two = kopen4load("two", 1);
    assert(one >= 0 && two >= 0 && one != two);
    assert(kfilelength(one) == 3);
    assert(kfilelength(two) == 2);

    char buf[8];
    assert(kread(one, buf, 1) == 1 && buf[0] == 'a');
    assert(kread(two, buf, sizeof(buf)) == 2 && !memcmp(buf, "de", 2));
    assert(kread(one, buf, sizeof(buf)) == 2 && !memcmp(buf, "bc", 2));
    assert(kread(one, buf, sizeof(buf)) == 0);
    assert(klseek(one, -1, SEEK_END) == 2);
    assert(klseek(one, INT32_MAX, SEEK_CUR) == -1);
    assert(klseek(one, -1, SEEK_SET) == -1);
    assert(kread(one, buf, 1) == 1 && buf[0] == 'c');
    assert(kread(one, buf, -1) == -1);
    assert(kread(-1, buf, 1) == -1);
    assert(kopen4load("ONE-extra", 1) == -1);

    for (int i = 2; i < DUKE_FILES; i++)
        assert(kopen4load("ONE", 1) >= 0);
    assert(kopen4load("ONE", 1) == -1);
    uninitgroupfile();
    uninitgroupfile();
    assert(files == 0 && groupefil_crc32[0] == 0);
    assert(unlink(path) == 0);
    return 0;
}
