/* PodLink library enumeration, file details and artwork. GPL version 2+. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "strcasecmp.h"
#include "file.h"
#include "mv.h"
#include "podlink_profile.h"
#include "rbpaths.h"
#include "version.h"
#include "screens/system/podlink_library.h"
#include "screens/system/podlink_media.h"
#ifdef HAVE_TAGCACHE
#include "crc32.h"
#include "database/tagcache.h"
#include "system/strutil.h"
#include "metadata/metadata_common.h"
#include "metadata/albumart.h"
#include "metadata/art_cache.h"

#define DB_WINDOW SEEK_LIST_SIZE
#define DATA_PREFIX "MS3 DATA . "
#define DATA_BYTES (((MEDIA_REPLY_SIZE - sizeof(DATA_PREFIX)) / 4) * 3)

static const int number_tags[] = {
    tag_year, tag_discnumber, tag_tracknumber, tag_length, tag_bitrate,
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_mtime
};

static const int text_tags[] = {
    tag_filename, tag_title, tag_artist, tag_album, tag_albumartist,
    tag_genre, tag_composer, tag_grouping, tag_comment
};

static enum { IDLE, COUNT, TRACKS, ART_TEXT, ART_BINARY, FILE_STREAM, SAMPLE_STREAM } operation;
static struct tagcache_search walk;
static int32_t commit_id;
static uint32_t generation;
static int slots, window_end, track_count, scan_removed;
static unsigned int record_len, record_offset;
static unsigned char record[ARRAYLEN(text_tags) * TAGCACHE_BUFSZ +
                            (ARRAYLEN(number_tags) + 2) * 12];
static char reply[MEDIA_REPLY_SIZE];
static int stream_fd;
static uint32_t stream_length, stream_offset, stream_crc, numeric_revision;

bool podlink_library_active(void)
{
    return operation != IDLE;
}

void podlink_library_close(void)
{
    if (operation == COUNT || operation == TRACKS)
        tagcache_search_finish(&walk);
    if (operation == ART_TEXT || operation == ART_BINARY ||
        operation == FILE_STREAM || operation == SAMPLE_STREAM)
        close(stream_fd);
    operation = IDLE;
}

static const char *fail(const char *message)
{
    podlink_library_close();
    return message;
}

static bool path_under(const char *path, const char *prefix, bool strict)
{
    size_t prefix_len = strlen(prefix);
    if (!strncmp(path, "/<HDD0>/", 8))
        path += 7;
    if (strncasecmp(path, prefix, prefix_len) ||
        strcspn(path, "\\:<>") != strlen(path))
        return false;

    path += prefix_len;
    while (*path)
    {
        size_t n = strcspn(path, "/");
        if (n == 0 || (n == 1 && path[0] == '.') ||
            (n == 2 && !memcmp(path, "..", 2)) ||
            (strict && (path[n - 1] == '.' || path[n - 1] == ' ')))
            return false;
        path += n;
        if (!*path)
            return true;
        path++;
    }
    return false;
}

static bool managed_path(const char *path)
{
    return path_under(path, PODLINK_MEDIA_DIR "/", false);
}

static bool sample_path(const char *path)
{
    return path_under(path, "/Music/", true) ||
           path_under(path, "/iPod_Control/Music/", true) ||
           path_under(path, REC_BASE_DIR "/", true);
}

static const char *data_reply(const unsigned char *data, unsigned int n,
                              bool final)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    memcpy(reply, DATA_PREFIX, sizeof(DATA_PREFIX) - 1);
    reply[sizeof("MS3 DATA ") - 1] = final ? '.' : '+';
    char *out = reply + sizeof(DATA_PREFIX) - 1;
    for (unsigned int i = 0; i < n; i += 3)
    {
        unsigned int a = data[i];
        unsigned int b = i + 1 < n ? data[i + 1] : 0;
        unsigned int c = i + 2 < n ? data[i + 2] : 0;
        *out++ = alphabet[a >> 2];
        *out++ = alphabet[((a & 3) << 4) | (b >> 4)];
        *out++ = i + 1 < n ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        *out++ = i + 2 < n ? alphabet[c & 63] : '=';
    }
    *out = '\0';
    return reply;
}

static const char *record_next(void)
{
    unsigned int n = MIN(DATA_BYTES, record_len - record_offset);
    const unsigned char *start = record + record_offset;
    record_offset += n;
    return data_reply(start, n, record_offset == record_len);
}

static bool collect_record(void)
{
    long values[ARRAYLEN(number_tags)];
    if (!tagcache_get_values(&walk, number_tags, values, ARRAYLEN(values)))
        return false;

    char *out = (char *)record;
    out += snprintf(out, 12, "%ld", (long)walk.idx_id) + 1;
    bool managed = false;
    for (unsigned int i = 0; i < ARRAYLEN(text_tags); i++)
    {
        if (!tagcache_retrieve(&walk, walk.idx_id, text_tags[i],
                               out, TAGCACHE_BUFSZ))
            return false;
        if (i == 0)
            managed = managed_path(out);
        out += strlen(out) + 1;
    }
    for (unsigned int i = 0; i < ARRAYLEN(values) - 1; i++)
        out += snprintf(out, 12, "%ld", values[i]) + 1;
    /* The stored timestamp is an unsigned 32-bit value. */
    out += snprintf(out, 12, "%lu", (unsigned long)(uint32_t)
                    values[ARRAYLEN(values) - 1]) + 1;
    *out++ = managed ? '1' : '0';
    *out++ = '\0';
    record_len = out - (char *)record;
    record_offset = 0;
    return true;
}

static bool database_busy(void)
{
    return !tagcache_search_ready() || tagcache_is_busy();
}

static const char *summary_reply(int count, int32_t revision,
                                 bool ready, bool busy)
{
    sector_t total, free;
    volume_size(IF_MV(0,) &total, &free);
    snprintf(reply, sizeof(reply), "MS3 SUMMARY %s\t%s\t%llu\t%llu\t%d"
             "\t%ld\t%d\t%d", TARGET_NAME, rbversion,
             (unsigned long long)total * 1024, (unsigned long long)free * 1024,
             count, (long)revision, ready, busy);
    return reply;
}

static bool database_changed(void)
{
    return database_busy() || !tagcache_is_usable() ||
           commit_id != tagcache_commit_id() ||
           generation != tagcache_generation() ||
           scan_removed != tagcache_get_stat()->scan_removed;
}

static bool file_snapshot(void)
{
    if (database_busy() || !tagcache_is_usable())
        return false;
    commit_id = tagcache_commit_id();
    generation = tagcache_generation();
    scan_removed = tagcache_get_stat()->scan_removed;
    return tagcache_export_revision(&numeric_revision) && !database_changed();
}

static bool file_snapshot_changed(void)
{
    uint32_t revision;
    return !tagcache_export_revision(&revision) || revision != numeric_revision ||
           database_changed();
}

static void stream_open(int fd, uint32_t length)
{
    stream_fd = fd;
    stream_length = length;
    stream_offset = 0;
    stream_crc = UINT32_MAX;
}

static const char *database_files(const unsigned char *name, size_t size)
{
    if (!file_snapshot())
        return "MS3 ERR database unavailable";

    int length = snprintf(reply, sizeof(reply), "MS3 FILES %ld\t%lu\t%d\t%lu",
                          (long)commit_id, (unsigned long)generation,
                          scan_removed, (unsigned long)numeric_revision);
    for (int tag = -1; tag < TAG_COUNT; tag++)
    {
        if (tag >= 0 && TAGCACHE_IS_NUMERIC(tag))
            continue;
        char basename[24], path[MAX_PATH];
        if (tag < 0)
            memcpy(basename, TAGCACHE_FILE_MASTER, sizeof(TAGCACHE_FILE_MASTER));
        else
            snprintf(basename, sizeof(basename), TAGCACHE_FILE_INDEX, tag);
        if (name && (strlen(basename) != size || memcmp(name, basename, size)))
            continue;

        int fd = open_pathfmt(path, sizeof(path), O_RDONLY, "%s/%s",
                              tagcache_get_stat()->db_path, basename);
        off_t bytes = fd < 0 ? -1 : ffilesize(fd);
        bool changed = file_snapshot_changed();
        if (changed || bytes < 0 || (uint64_t)bytes > UINT32_MAX)
        {
            if (fd >= 0)
                close(fd);
            return changed ? "MS3 ERR database changed" : "MS3 ERR database read";
        }
        if (name)
        {
            stream_open(fd, bytes);
            operation = FILE_STREAM;
            snprintf(reply, sizeof(reply), "MS3 FILE %lu", (unsigned long)bytes);
            return reply;
        }

        close(fd);
        int n = snprintf(reply + length, sizeof(reply) - length, "\t%s:%lu",
                         basename, (unsigned long)bytes);
        if (n < 0 || (size_t)n >= sizeof(reply) - length)
            return "MS3 ERR database manifest";
        length += n;
    }
    if (name)
        return "MS3 ERR database file";
    if (file_snapshot_changed())
        return "MS3 ERR database changed";
    if ((unsigned int)length > podlink_max_message())
        return "MS3 ERR message size";
    return reply;
}

static const char *binary_next(unsigned char *binary, unsigned int *size,
                               uint32_t *raw)
{
    bool database = operation == FILE_STREAM;
    if (database && file_snapshot_changed())
        return fail("MS3 ERR database changed");
    if (stream_offset == stream_length)
    {
        snprintf(reply, sizeof(reply), "MS3 DONE %lu %08lx",
                 (unsigned long)stream_length,
                 (unsigned long)(stream_crc ^ UINT32_MAX));
        return fail(reply);
    }

    unsigned int limit = podlink_max_message();
    if (limit <= 8)
        return fail("MS3 ERR message size");
    /* The CDC TX ring holds 16 KiB. Include PLNK framing so that a
     * full chunk fits in one ring fill even with a 16 KiB payload offer. */
    limit = MIN(limit, PODLINK_CDC_RING_BYTES -
                       (PODLINK_FRAME_MAX - PODLINK_MAX_MESSAGE));
    unsigned int n = MIN(limit - 8, stream_length - stream_offset);
    memcpy(binary, "MSF1", 4);
    for (unsigned int i = 0; i < 4; i++)
        binary[4 + i] = stream_offset >> (8 * i);
    uint32_t started = podlink_profile_now();
    ssize_t bytes = read(stream_fd, binary + 8, n);
    podlink_profile_add(PL_READ, started, bytes > 0 ? bytes : 0);
    if (bytes != (ssize_t)n)
        return fail(database ? "MS3 ERR database read" :
                    operation == SAMPLE_STREAM ? "MS3 ERR sample read" :
                    "MS3 ERR artwork read");
    if (database && file_snapshot_changed())
        return fail("MS3 ERR database changed");

    started = podlink_profile_now();
    stream_crc = podlink_bulk_crc(binary + 8, n, stream_crc, raw);
    podlink_profile_add(PL_FILE_CRC, started, n);
    stream_offset += n;
    *size = n + 8;
    return NULL;
}

static const char *database_next(void)
{
    if (database_changed())
        return fail("MS3 ERR database changed");
    if (operation == TRACKS && record_offset < record_len)
        return record_next();

    char number[16];
    while (tagcache_get_next(&walk, number, sizeof(number)))
    {
        if (operation == TRACKS)
        {
            bool ok = collect_record();
            if (database_changed())
                return fail("MS3 ERR database changed");
            return ok ? record_next() : fail("MS3 ERR database read");
        }
        track_count++;
    }
    if (database_changed())
        return fail("MS3 ERR database changed");
    if (walk.failed)
        return fail("MS3 ERR database read");

    if (window_end >= slots)
    {
        bool count = operation == COUNT;
        podlink_library_close();
        return count ? summary_reply(track_count, commit_id, true, false) :
                       "MS3 END";
    }

    /* Numeric searches walk the master index in either cache mode.
     * Limit the range even when every slot in the window is deleted. */
    int first = window_end;
    window_end += MIN(DB_WINDOW, slots - window_end);
    walk.valid = true;
    tagcache_search_set_range(&walk, first, window_end - 1);
    return "MS3 MORE";
}

static const char *database_open(bool count)
{
    bool busy = database_busy();
    if (busy || !tagcache_is_usable())
        return count ? summary_reply(0, tagcache_commit_id(), false, busy) :
                       "MS3 ERR database unavailable";

    commit_id = tagcache_commit_id();
    generation = tagcache_generation();
    scan_removed = tagcache_get_stat()->scan_removed;
    if (!tagcache_search(&walk, tag_length))
        return "MS3 ERR database unavailable";
    operation = count ? COUNT : TRACKS;
    slots = tagcache_get_stat()->total_entries;
    track_count = 0;
    record_len = record_offset = 0;
    window_end = MIN(DB_WINDOW, slots);
    tagcache_search_set_range(&walk, 0, window_end - 1);
    return count ? database_next() : "MS3 READY";
}

static const char *art_next(void)
{
    unsigned char buffer[DATA_BYTES];
    unsigned int n = MIN(sizeof(buffer), stream_length - stream_offset);
    if (read(stream_fd, buffer, n) != (ssize_t)n)
        return fail("MS3 ERR artwork read");
    stream_offset += n;
    bool final = stream_offset == stream_length;
    if (final)
        podlink_library_close();
    return data_reply(buffer, n, final);
}

static const char *art_open(const char *path, int size, bool binary)
{
    char thumbnail[MAX_PATH];
    bool fallback;
    if (!art_cache_lookup_hash(art_cache_track_key(path), size,
                               thumbnail, sizeof(thumbnail), &fallback))
        return "MS3 NOART";
    if (fallback)
        return "MS3 PLACEHOLDER";

    int fd = open(thumbnail, O_RDONLY);
    if (fd < 0)
        return "MS3 ERR artwork read";

    struct art_cache_header header;
    if (read(fd, &header, sizeof(header)) != sizeof(header) ||
        header.magic != ART_CACHE_MAGIC ||
        header.version != ART_CACHE_FORMAT_VERSION ||
        !header.width || header.width > art_cache_size_dim(size) ||
        !header.height || header.height > art_cache_size_dim(size) ||
        (header.layout != AA_ROWS && header.layout != AA_COLUMNS) ||
        ffilesize(fd) != (off_t)(sizeof(header) +
                                (unsigned int)header.width * header.height * 2))
    {
        close(fd);
        return "MS3 ERR artwork read";
    }

    stream_open(fd, (unsigned int)header.width * header.height * 2);
    operation = binary ? ART_BINARY : ART_TEXT;
    snprintf(reply, sizeof(reply), "MS3 %s %u %u %s",
             binary ? "ARTBIN" : "ART", header.width,
             header.height, header.layout == AA_COLUMNS ? "columns" : "rows");
    return reply;
}

static bool read_at(int fd, off_t pos, void *buffer, size_t size)
{
    return lseek(fd, pos, SEEK_SET) == pos &&
           read(fd, buffer, size) == (ssize_t)size;
}

static const unsigned char *descriptor(const unsigned char *p,
                                       const unsigned char **end, int tag)
{
    if (p == *end || *p++ != tag)
        return NULL;
    unsigned int length = 0;
    for (unsigned int i = 0; i < 4 && p < *end; i++)
    {
        unsigned int byte = *p++;
        length = (length << 7) | (byte & 127);
        if (!(byte & 128))
        {
            if (length > (size_t)(*end - p))
                return NULL;
            *end = p + length;
            return p;
        }
    }
    return NULL;
}

static unsigned int aac_channels(int fd, off_t pos, size_t size)
{
    unsigned char buf[128];
    size = MIN(size, sizeof(buf));
    if (!read_at(fd, pos, buf, size) || size < 4)
        return 0;
    const unsigned char *end = buf + size;
    const unsigned char *p = descriptor(buf + 4, &end, 3);
    if (!p || end - p < 3)
        return 0;
    unsigned int flags = p[2];
    p += 3;
    if (flags & 128)
    {
        if (end - p < 2)
            return 0;
        p += 2;
    }
    if (flags & 64)
    {
        if (p == end || end - p <= *p)
            return 0;
        p += 1 + *p;
    }
    if (flags & 32)
    {
        if (end - p < 2)
            return 0;
        p += 2;
    }
    p = descriptor(p, &end, 4);
    if (!p || end - p < 13)
        return 0;
    p = descriptor(p + 13, &end, 5);
    if (!p || end - p < 2 || p[0] >> 3 == 31)
        return 0;
    unsigned int config = (p[1] >> 3) & 15;
    /* Explicit frequency inserts 24 bits before channelConfiguration. */
    if (((p[0] & 7) << 1 | p[1] >> 7) == 15)
    {
        if (end - p < 5)
            return 0;
        config = (p[4] >> 3) & 15;
    }
    static const unsigned char channels[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
    return config < ARRAYLEN(channels) ? channels[config] : 0;
}

static unsigned int mp4_channels(int fd, off_t pos, off_t end,
                                 unsigned int *budget, bool alac, int depth)
{
    unsigned char header[8];
    while (*budget && end - pos >= 8)
    {
        --*budget;
        if (!read_at(fd, pos, header, sizeof(header)))
            break;
        uint32_t size = get_long_be(header);
        if (size < 8 || size > (uint64_t)(end - pos))
            break;
        off_t next = pos + size;
        off_t payload = pos + 8;
        const unsigned char *type = header + 4;
        unsigned int skip = 0;
        if (!memcmp(type, "esds", 4) && !alac)
            return aac_channels(fd, payload, next - payload);
        if (!memcmp(type, "alac", 4) && alac && size == 36)
        {
            unsigned char channels;
            return read_at(fd, payload + 13, &channels, 1) ? channels : 0;
        }
        if (!memcmp(type, "stsd", 4))
            skip = 8;
        else if (!memcmp(type, "mp4a", 4) || !memcmp(type, "alac", 4))
            skip = 28;
        else if (memcmp(type, "moov", 4) && memcmp(type, "trak", 4) &&
                 memcmp(type, "mdia", 4) && memcmp(type, "minf", 4) &&
                 memcmp(type, "stbl", 4))
        {
            pos = next;
            continue;
        }

        /* Only descend through the fixed sample-description hierarchy. */
        if (depth < 8 && next - payload >= (off_t)skip)
        {
            unsigned int n = mp4_channels(fd, payload + skip, next,
                                           budget, alac, depth + 1);
            if (n)
                return n;
        }
        pos = next;
    }
    return 0;
}

static unsigned int pcm_channels(int fd, off_t size, bool wave)
{
    unsigned char b[12];
    if (!read_at(fd, 0, b, sizeof(b)) ||
        memcmp(b, wave ? "RIFF" : "FORM", 4))
        return 0;

    off_t pos = 12;
    for (unsigned int i = 0; i < 32 && size - pos >= 8; i++)
    {
        if (!read_at(fd, pos, b, 8))
            break;
        uint32_t length = wave ? get_long_le(b + 4) : get_long_be(b + 4);
        off_t payload = pos + 8;
        if (length > (uint64_t)(size - payload))
            break;
        if (!memcmp(b, wave ? "fmt " : "COMM", 4))
        {
            if (length < 4 || !read_at(fd, payload, b, 4))
                break;
            return wave ? get_short_le(b + 2) : get_short_be(b);
        }
        if ((uint64_t)length + (length & 1) > (uint64_t)(size - payload))
            break;
        pos = payload + length + (length & 1);
    }
    return 0;
}

static unsigned int file_channels(int fd, off_t size,
                                  const struct mp3entry *id3)
{
    unsigned char b[32];
    unsigned int budget = 64;
    switch (id3->codectype)
    {
    case AFMT_MPA_L1:
    case AFMT_MPA_L2:
    case AFMT_MPA_L3:
        return read_at(fd, id3->first_frame_offset, b, 4) &&
               b[0] == 0xff && (b[1] & 0xe0) == 0xe0 ?
               ((b[3] >> 6) == 3 ? 1 : 2) : 0;
    case AFMT_FLAC:
        return read_at(fd, id3->id3v2len, b, 21) && !memcmp(b, "fLaC", 4) &&
               !(b[4] & 127) ? ((b[20] >> 1) & 7) + 1 : 0;
    case AFMT_MP4_ALAC:
    case AFMT_MP4_AAC:
    case AFMT_MP4_AAC_HE:
        return mp4_channels(fd, 0, size, &budget,
                            id3->codectype == AFMT_MP4_ALAC, 0);
    case AFMT_PCM_WAV:
    case AFMT_AIFF:
        return pcm_channels(fd, size, id3->codectype == AFMT_PCM_WAV);
    case AFMT_OGG_VORBIS:
    case AFMT_OPUS:
        if (read_at(fd, 0, b, 27) && !memcmp(b, "OggS", 4) &&
            read_at(fd, 27 + b[26], b, 12))
        {
            if (!memcmp(b, "\1vorbis", 7))
                return b[11]; /* Version precedes the channel count. */
            if (!memcmp(b, "OpusHead", 8))
                return b[9];
        }
        break;
    }
    return 0;
}

static const char *file_detail(const char *path)
{
    static struct mp3entry id3;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return "MS3 ERR file read";
    off_t size = ffilesize(fd);
    bool ok = size >= 0 && get_metadata(&id3, fd, path);
    unsigned int channels = ok ? file_channels(fd, size, &id3) : 0;
    close(fd);
    if (!ok)
        return "MS3 ERR metadata";

    char art[MAX_PATH];
    bool has_art = id3.has_embedded_albumart ||
                   art_folder_file(path, art, sizeof(art));
    snprintf(reply, sizeof(reply), "MS3 DETAIL %llu\t%s\t%lu\t%u\t%d",
             (unsigned long long)size, get_codec_string(id3.codectype),
             id3.frequency, channels, has_art);
    return reply;
}

const char *podlink_library_command(const unsigned char *data, size_t size,
                                   bool *refresh_pending, unsigned char *binary,
                                   unsigned int *binary_size, uint32_t *raw)
{
    if (size == 4 && !memcmp(data, "STOP", 4))
        return fail("MS3 STOPPED");
    if (size == 4 && !memcmp(data, "NEXT", 4))
    {
        if (operation == FILE_STREAM || operation == ART_BINARY ||
            operation == SAMPLE_STREAM)
            return binary_next(binary, binary_size, raw);
        if (operation == ART_TEXT)
            return art_next();
        if (operation != IDLE)
            return database_next();
        return "MS3 ERR state";
    }
    if (operation != IDLE)
        return "MS3 ERR busy";

    if (size == 7 && !memcmp(data, "SUMMARY", 7))
        return database_open(true);
    if (size == 2 && !memcmp(data, "DB", 2))
        return database_open(false);

    if (size == 5 && !memcmp(data, "FILES", 5))
        return database_files(NULL, 0);
    if (size >= 5 && !memcmp(data, "FILE ", 5))
        return database_files(data + 5, size - 5);

    int art_size = -1;
    bool binary_art = size >= 7 && !memcmp(data, "ARTBIN ", 7);
    bool delete = size > 4 && !memcmp(data, "DEL ", 4);
    bool sample = size > 7 && !memcmp(data, "SAMPLE ", 7);
    if (sample)
    {
        data += 7;
        size -= 7;
    }
    else if (size > 7 && !memcmp(data, "DETAIL ", 7))
    {
        data += 7;
        size -= 7;
    }
    else if (delete)
    {
        data += 4;
        size -= 4;
    }
    else if (binary_art || (size > 4 && !memcmp(data, "ART ", 4)))
    {
        char name[10];
        unsigned int prefix = binary_art ? 7 : 4;
        data += prefix;
        size -= prefix;
        const unsigned char *end = memchr(data, ' ', size);
        if (!end || (size_t)(end - data) >= sizeof(name))
            return "MS3 ERR artwork size";
        memcpy(name, data, end - data);
        name[end - data] = '\0';
        art_size = art_cache_size_index(name);
        if (art_size < 0)
            return "MS3 ERR artwork size";
        size -= end + 1 - data;
        data = end + 1;
    }
    else
        return "MS3 ERR command";

    char path[MAX_PATH + 1];
    if (!size || size >= sizeof(path) || data[0] != '/' ||
        memchr(data, '\0', size))
        return "MS3 ERR path";
    memcpy(path, data, size);
    path[size] = '\0';
    if (sample)
    {
        if (!sample_path(path) || !file_exists(path))
            return "MS3 ERR sample path";
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return "MS3 ERR sample read";
        off_t length = ffilesize(fd);
        if (length <= 0 || (uint64_t)length > 128u * 1024u * 1024u)
        {
            close(fd);
            return "MS3 ERR sample size";
        }
        stream_open(fd, length);
        operation = SAMPLE_STREAM;
        snprintf(reply, sizeof(reply), "MS3 SAMPLE %lu", (unsigned long)length);
        return reply;
    }
    if (delete)
    {
        if (!managed_path(path))
            return "MS3 ERR unmanaged path";
        if (remove(path) < 0)
            return "MS3 ERR delete";
        *refresh_pending = true;
        return "MS3 DELETED";
    }
    if (art_size >= 0)
        return art_open(path, art_size, binary_art);
    return file_detail(path);
}
#else
bool podlink_library_active(void)
{
    return false;
}

void podlink_library_close(void)
{
}

const char *podlink_library_command(const unsigned char *data, size_t size,
                                   bool *refresh_pending, unsigned char *binary,
                                   unsigned int *binary_size, uint32_t *raw)
{
    (void)data;
    (void)size;
    (void)refresh_pending;
    (void)binary;
    (void)binary_size;
    (void)raw;
    return "MS3 ERR database unavailable";
}
#endif
