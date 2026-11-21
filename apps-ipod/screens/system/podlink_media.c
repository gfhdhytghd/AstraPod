/* PodLink media import. GPL version 2 or later. */
#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "crc32.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "podlink_media.h"
#include "podlink_library.h"
#include "system/bg_task.h"
#ifdef HAVE_TAGCACHE
#include "database/tagcache.h"
#endif

#define SYNC_DIR PODLINK_MEDIA_DIR "/Jellyfin"
#define STAGING_DIR "/.rockbox/podlink-media"
#define STAGING_PATH STAGING_DIR "/pending.part"
#define MAX_TRACK_BYTES (128u * 1024u * 1024u)
#define NAME_HASH_LENGTH 64

static int track_fd = -1;
static unsigned int remaining;
static bool refresh_pending;
static uint32_t checksum, expected_checksum;
static char destination[sizeof(SYNC_DIR) + NAME_HASH_LENGTH + 6];
static char inventory_reply[MEDIA_REPLY_SIZE];
static DIR *inventory;

static void close_inventory(void)
{
    if (inventory)
    {
        closedir(inventory);
        inventory = NULL;
    }
}

#ifdef HAVE_TAGCACHE
static struct tagcache_search local_music;
static bool local_open;
static int32_t local_revision;

static void close_local_music(void)
{
    if (local_open)
    {
        tagcache_search_finish(&local_music);
        local_open = false;
    }
}

static const char *open_local_music(void)
{
    close_local_music();
    if (!tagcache_is_usable() || !tagcache_search_ready() || tagcache_is_busy())
        return "MS2 ERR database unavailable";
    local_open = tagcache_search(&local_music, tag_filename);
    if (!local_open)
    {
        tagcache_search_finish(&local_music);
        return "MS2 ERR database unavailable";
    }
    local_revision = tagcache_commit_id();
    return "MS2 DBREADY";
}

static bool usable_tag(const char *tag, size_t capacity)
{
    return tag[0] && strlen(tag) < capacity - 1 &&
           !strchr(tag, '\t') && !strchr(tag, '\r') && !strchr(tag, '\n');
}

static const char *next_local_music(void)
{
    static const int tags[] = { tag_length, tag_discnumber, tag_tracknumber };
    if (!local_open)
        return "MS2 ERR database state";

    /* A bounded step keeps a run of incomplete records from holding USB. */
    for (unsigned int count = 0; count < 32; ++count)
    {
        if (!tagcache_search_ready() || tagcache_is_busy() ||
            local_revision != tagcache_commit_id())
        {
            close_local_music();
            return "MS2 ERR database changed";
        }
        static char path[MAX_PATH];
        static char title[TAGCACHE_BUFSZ], artist[TAGCACHE_BUFSZ], album[TAGCACHE_BUFSZ];
        if (!tagcache_get_next(&local_music, path, sizeof(path)))
        {
            bool failed = local_music.failed;
            close_local_music();
            return failed ? "MS2 ERR database read" : "MS2 DBDONE";
        }
        if (strlen(path) >= sizeof(path) - 1 || strstr(path, SYNC_DIR "/") ||
            !file_exists(path))
            continue;

        long values[3];
        if (!tagcache_get_values(&local_music, tags, values, 3) ||
            !tagcache_retrieve_two(&local_music, local_music.idx_id,
                                   tag_title, title, sizeof(title),
                                   tag_artist, artist, sizeof(artist)) ||
            !tagcache_retrieve(&local_music, local_music.idx_id,
                               tag_album, album, sizeof(album)))
        {
            close_local_music();
            return "MS2 ERR database read";
        }
        if (values[0] <= 0 || !usable_tag(title, sizeof(title)) ||
            !usable_tag(artist, sizeof(artist)) || !usable_tag(album, sizeof(album)))
            continue;
        int length = snprintf(inventory_reply, sizeof(inventory_reply),
                              "MS2 TRACK %ld\t%ld\t%ld\t%s\t%s\t%s",
                              values[0], values[1], values[2], title, artist, album);
        if (length > 0 && (size_t)length < sizeof(inventory_reply))
            return inventory_reply;
    }
    return "MS2 DBMORE";
}
#endif

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

static void reset_files(void)
{
    abort_track();
    close_inventory();
#ifdef HAVE_TAGCACHE
    close_local_music();
#endif
    podlink_library_close();
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

static bool valid_name(const unsigned char *name, unsigned int length)
{
    if (length < NAME_HASH_LENGTH + 4 ||
        !hex_value(name, NAME_HASH_LENGTH, NULL))
        return false;
    const unsigned char *suffix = name + NAME_HASH_LENGTH;
    unsigned int suffix_size = length - NAME_HASH_LENGTH;
    return (suffix_size == 4 && (!memcmp(suffix, ".mp3", 4) ||
                                 !memcmp(suffix, ".m4a", 4))) ||
           (suffix_size == 5 && !memcmp(suffix, ".flac", 5));
}

static const char *begin(const unsigned char *data, unsigned int size, bool sync)
{
    const unsigned int header_size = 18;
    if (size <= header_size || data[8] != ' ' || data[17] != ' ')
        return "MS1 ERR header";

    uint32_t bytes, crc;
    const unsigned char *name = data + header_size;
    unsigned int length = size - header_size;
    if (!hex_value(data, 8, &bytes) || !bytes || bytes > MAX_TRACK_BYTES ||
        !hex_value(data + 9, 8, &crc) || !valid_name(name, length))
        return "MS1 ERR header";

    snprintf(destination, sizeof(destination), "%s/%.*s",
             sync ? SYNC_DIR : PODLINK_MEDIA_DIR, (int)length, (const char *)name);

    if (file_exists(destination))
        return "MS1 ERR exists";

    mkdir("/Music");
    mkdir(PODLINK_MEDIA_DIR);
    if (sync)
        mkdir(SYNC_DIR);
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

static const char *sync_command(const unsigned char *data, unsigned int size)
{
    if (track_fd >= 0)
        return "MS2 ERR busy";
#ifdef HAVE_TAGCACHE
    if (size == 6 && !memcmp(data, "MS2 DB", 6) && !inventory)
        return open_local_music();
    if (size == 10 && !memcmp(data, "MS2 DBNEXT", 10))
        return next_local_music();
    if (local_open)
        return "MS2 ERR busy";
#endif
    if (size == 8 && !memcmp(data, "MS2 LIST", 8))
    {
        close_inventory();
        inventory = opendir(SYNC_DIR);
        if (inventory)
            return "MS2 READY";
        return errno == ENOENT ? "MS2 DONE" : "MS2 ERR inventory";
    }
    if (size == 8 && !memcmp(data, "MS2 NEXT", 8) && inventory)
    {
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(inventory)))
        {
            const char *name = entry->d_name;
            unsigned int length = strlen(name);
            if (valid_name((const unsigned char *)name, length) &&
                !(dir_get_info(inventory, entry).attribute & ATTR_DIRECTORY))
            {
                const unsigned int prefix = sizeof("MS2 FILE ") - 1;
                memcpy(inventory_reply, "MS2 FILE ", prefix);
                memcpy(inventory_reply + prefix, name, length + 1);
                return inventory_reply;
            }
        }
        int error = errno;
        close_inventory();
        return error ? "MS2 ERR inventory" : "MS2 DONE";
    }

    if (inventory)
        return "MS2 ERR busy";
    if (size > 8 && !memcmp(data, "MS2 PUT ", 8))
        return begin(data + 8, size - 8, true);
    if (size > 8 && !memcmp(data, "MS2 DEL ", 8) && valid_name(data + 8, size - 8))
    {
        char path[sizeof(destination)];
        snprintf(path, sizeof(path), SYNC_DIR "/%.*s", (int)size - 8,
                 (const char *)data + 8);
        if (!file_exists(path) || remove(path) < 0)
            return "MS2 ERR delete";
        refresh_pending = true;
        return "MS2 DELETED";
    }
    return "MS2 ERR command";
}

static const char *handle_message(const struct podlink_message *message)
{
    const unsigned char *data = message->data;
    unsigned int size = message->length;
    if (message->type == PODLINK_BINARY && size >= 4 &&
        !memcmp(data, "MSD1", 4))
    {
        if (podlink_library_active())
            return "MS1 ERR busy";
        if (track_fd < 0)
            return "MS1 ERR state";
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

    if (message->type == PODLINK_TEXT && size >= 4 && !memcmp(data, "MS3 ", 4))
    {
        if (track_fd >= 0 || inventory
#ifdef HAVE_TAGCACHE
            || local_open
#endif
            )
            return "MS3 ERR busy";
        return podlink_library_command(data + 4, size - 4, &refresh_pending);
    }
    if (message->type == PODLINK_TEXT && size >= 4 && !memcmp(data, "MS2 ", 4))
        return podlink_library_active() ? "MS2 ERR busy" : sync_command(data, size);
    if (message->type != PODLINK_TEXT || size < 4 || memcmp(data, "MS1 ", 4))
        return track_fd >= 0 ? "MS1 ERR state" : NULL;
    if (podlink_library_active())
        return "MS1 ERR busy";
    if (size == 11 && !memcmp(data, "MS1 REFRESH", 11) && track_fd < 0)
    {
        refresh_library();
        return "MS1 QUEUED";
    }
    if (size == 8 && !memcmp(data, "MS1 STOP", 8))
    {
        reset_files();
        return "MS1 STOPPED";
    }
    if (size == 7 && !memcmp(data, "MS1 END", 7) && track_fd >= 0)
        return finish();
    if (size > 8 && !memcmp(data, "MS1 PUT ", 8) && track_fd < 0)
        return begin(data + 8, size - 8, false);
    return "MS1 ERR state";
}

/* Only the storage worker touches file descriptors. The transport callback
 * owns a single bounded mailbox and never waits for disk I/O. */
static struct mutex media_mutex;
static struct podlink_message pending_message;
static unsigned char mailbox[PODLINK_MAX_MESSAGE];
static char pending_reply[sizeof(inventory_reply)];
static bool queued, working, reset_pending;

void podlink_media_init(void)
{
    mutex_init(&media_mutex);
}

bool podlink_media_enqueue(const struct podlink_message *message)
{
    if (message->length < 4 || message->length > sizeof(mailbox) ||
        !((message->type == PODLINK_TEXT &&
           (!memcmp(message->data, "MS1 ", 4) ||
            !memcmp(message->data, "MS2 ", 4) ||
            !memcmp(message->data, "MS3 ", 4))) ||
          (message->type == PODLINK_BINARY && !memcmp(message->data, "MSD1", 4))))
        return false;

    mutex_lock(&media_mutex);
    if (!queued && !working && !pending_reply[0])
    {
        memcpy(mailbox, message->data, message->length);
        pending_message = *message;
        pending_message.data = mailbox;
        queued = true;
    }
    mutex_unlock(&media_mutex);
    return true;
}

void podlink_media_disconnect(void)
{
    mutex_lock(&media_mutex);
    queued = false;
    pending_reply[0] = '\0';
    reset_pending = true;
    mutex_unlock(&media_mutex);
}

unsigned int podlink_media_next(char *reply, unsigned int size)
{
    mutex_lock(&media_mutex);
    unsigned int length = strlen(pending_reply);
    if (length <= size)
    {
        memcpy(reply, pending_reply, length);
        pending_reply[0] = '\0';
    }
    else
        length = 0;
    mutex_unlock(&media_mutex);
    return length;
}

void podlink_media_work(void)
{
    mutex_lock(&media_mutex);
    bool reset = reset_pending;
    bool process = queued;
    reset_pending = queued = false;
    working = process;
    mutex_unlock(&media_mutex);

    if (reset)
        reset_files();
    if (!process)
        return;

    const char *reply = handle_message(&pending_message);
    mutex_lock(&media_mutex);
    if (!reset_pending && reply)
        snprintf(pending_reply, sizeof(pending_reply), "%s", reply);
    working = false;
    mutex_unlock(&media_mutex);
}
