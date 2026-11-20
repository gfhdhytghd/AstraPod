/* PodLink media import. GPL version 2 or later. */
#include "config.h"
#include <string.h>
#include "crc32.h"
#include "file.h"
#include "dir.h"
#include "podlink_media.h"
#include "system/bg_task.h"

#define MEDIA_DIR "/Music/PodLink"
#define STAGING_DIR "/.rockbox/podlink-media"
#define STAGING_PATH STAGING_DIR "/pending.part"
#define MAX_TRACK_BYTES (128u * 1024u * 1024u)
#define NAME_HASH_LENGTH 64

static int track_fd = -1;
static unsigned int remaining;
static bool refresh_pending;
static uint32_t checksum, expected_checksum;
static char destination[sizeof(MEDIA_DIR) + NAME_HASH_LENGTH + 6];

static void abort_track(void)
{
    if (track_fd >= 0)
    {
        close(track_fd);
        track_fd = -1;
        remove(STAGING_PATH);
    }
}

static void refresh_library(void)
{
#ifdef HAVE_TAGCACHE
    /* The art cache watches the tag database commit and refreshes once
     * the new marks settle. */
    bg_task_update(&tagcache_task);
#endif
    refresh_pending = false;
}

void podlink_media_reset(void)
{
    abort_track();
    if (refresh_pending)
        refresh_library();
}

static bool hex_value(const unsigned char *text, unsigned int count,
                      uint32_t *value)
{
    uint32_t result = 0;
    while (count--)
    {
        unsigned char c = *text++;
        if (c >= '0' && c <= '9')
            result = (result << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f')
            result = (result << 4) | (c - 'a' + 10);
        else
            return false;
    }
    if (value)
        *value = result;
    return true;
}

static const char *begin(const unsigned char *data, unsigned int size)
{
    /* The path is selected by the firmware. The host supplies only a hash
     * and one supported suffix. Length and CRC use eight hex digits each. */
    const unsigned int header_size = 18;
    if (size < header_size + NAME_HASH_LENGTH + 1 ||
        data[8] != ' ' || data[17] != ' ')
        return "MS1 ERR header";

    uint32_t bytes, crc;
    const unsigned char *name = data + header_size;
    if (!hex_value(data, 8, &bytes) || !bytes || bytes > MAX_TRACK_BYTES ||
        !hex_value(data + 9, 8, &crc) ||
        !hex_value(name, NAME_HASH_LENGTH, NULL))
        return "MS1 ERR header";

    const unsigned char *suffix = name + NAME_HASH_LENGTH;
    unsigned int suffix_size = size - header_size - NAME_HASH_LENGTH;
    if (!((suffix_size == 4 && (!memcmp(suffix, ".mp3", 4) ||
                                !memcmp(suffix, ".m4a", 4))) ||
          (suffix_size == 5 && !memcmp(suffix, ".flac", 5))))
        return "MS1 ERR suffix";

    memcpy(destination, MEDIA_DIR "/", sizeof(MEDIA_DIR));
    memcpy(destination + sizeof(MEDIA_DIR), name, NAME_HASH_LENGTH + suffix_size);
    destination[sizeof(MEDIA_DIR) + NAME_HASH_LENGTH + suffix_size] = '\0';

    if (file_exists(destination))
        return "MS1 ERR exists";

    mkdir("/Music");
    mkdir(MEDIA_DIR);
    mkdir(STAGING_DIR);
    track_fd = open(STAGING_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (track_fd < 0)
        return "MS1 ERR staging";

    remaining = bytes;
    expected_checksum = crc;
    checksum = UINT32_MAX;
    return "MS1 READY";
}

static const char *finish(void)
{
    if (remaining || (checksum ^ UINT32_MAX) != expected_checksum)
    {
        abort_track();
        return "MS1 ERR integrity";
    }
    int synced = fsync(track_fd);
    int closed = close(track_fd);
    track_fd = -1;
    if (synced < 0 || closed < 0 || file_exists(destination) ||
        rename(STAGING_PATH, destination) < 0)
    {
        remove(STAGING_PATH);
        return "MS1 ERR publish";
    }
    refresh_pending = true;
    return "MS1 STORED";
}

const char *podlink_media_receive(const struct podlink_message *message,
                                  bool link_idle)
{
    const unsigned char *data = message->data;
    unsigned int size = message->length;
    if (message->type == PODLINK_BINARY && size >= 4 &&
        !memcmp(data, "MSD1", 4))
    {
        if (track_fd < 0)
            return link_idle ? "MS1 ERR state" : NULL;
        data += 4;
        size -= 4;
        if (!size || size > remaining || write(track_fd, data, size) != (int)size)
        {
            abort_track();
            return "MS1 ERR write";
        }
        checksum = crc_32r(data, size, checksum);
        remaining -= size;
        return "MS1 ACK";
    }

    if (message->type != PODLINK_TEXT || size < 4 || memcmp(data, "MS1 ", 4))
        return track_fd >= 0 ? "MS1 ERR state" : NULL;
    if (!link_idle)
        return "MS1 ERR busy";
    if (size == 11 && !memcmp(data, "MS1 REFRESH", 11) && track_fd < 0)
    {
        refresh_library();
        return "MS1 QUEUED";
    }
    if (size == 8 && !memcmp(data, "MS1 STOP", 8))
    {
        podlink_media_reset();
        return "MS1 STOPPED";
    }
    if (size == 7 && !memcmp(data, "MS1 END", 7) && track_fd >= 0)
        return finish();
    if (size > 8 && !memcmp(data, "MS1 PUT ", 8) && track_fd < 0)
        return begin(data + 8, size - 8);
    return "MS1 ERR state";
}
