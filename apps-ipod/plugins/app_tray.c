/* AstraPod persistent app reminders. GPL version 2 or later. */
#include "plugins/app_tray.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "timefuncs.h"

#define TRAY_PATH ROCKBOX_DIR "/app-tray.dat"
#define TRAY_TEMP TRAY_PATH ".tmp"
#define TRAY_SLOTS 4
#define TRAY_OWNER_SIZE 32

/* V2 disk layout for little-endian AstraPod targets. Zero is an invalid RTC
 * stamp. The schedule is pure data so it can outlive the plugin arena. */
static const char signature[8] = "ASTRY02";

struct tray_slot {
    char owner[TRAY_OWNER_SIZE];
    uint32_t start, delays[ROCK_TRAY_MAX_RULES];
    struct rock_tray_icon12 frames[ROCK_TRAY_MAX_FRAMES];
    uint8_t targets[ROCK_TRAY_MAX_RULES];
    uint8_t count, frame, steps, reserved;
};

struct tray_store {
    char magic[sizeof(signature)];
    struct tray_slot slots[TRAY_SLOTS];
    uint32_t crc;
};

_Static_assert(sizeof(struct tray_store) == 636, "tray file layout");

/* V1 is V2 with 8 x 8 frames; it is read once and widened. */
static const char signature_v1[8] = "ASTRY01";

struct tray_slot_v1 {
    char owner[TRAY_OWNER_SIZE];
    uint32_t start, delays[ROCK_TRAY_MAX_RULES];
    struct rock_tray_bitmap frames[ROCK_TRAY_MAX_FRAMES];
    uint8_t targets[ROCK_TRAY_MAX_RULES];
    uint8_t count, frame, steps, reserved;
};

struct tray_store_v1 {
    char magic[sizeof(signature_v1)];
    struct tray_slot_v1 slots[TRAY_SLOTS];
    uint32_t crc;
};

_Static_assert(sizeof(struct tray_store_v1) == 380, "V1 tray file layout");

static struct tray_store store;
static struct mutex store_lock;
static bool ready;

static uint32_t tray_clock(const struct tm *now)
{
    if (!valid_time(now))
        return 0;

    /* Civil seconds from 2000, independent of time_t width and host DST. */
    static const unsigned month_days[] =
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    unsigned year = now->tm_year - 100;
    unsigned days = year * 365 + (year + 3) / 4 + month_days[now->tm_mon];
    days += now->tm_mon >= 2 && year % 4 == 0;
    days += now->tm_mday - 1;
    return 1 + days * 86400u + now->tm_hour * 3600u +
           now->tm_min * 60u + now->tm_sec;
}

static bool owner_valid(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_';
}

static bool owner_from_path(const char *path, char owner[TRAY_OWNER_SIZE])
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    if (!len || len >= TRAY_OWNER_SIZE)
        return false;

    memset(owner, 0, TRAY_OWNER_SIZE);
    for (size_t i = 0; i < len; ++i)
    {
        char ch = base[i];
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
        if (!owner_valid(ch))
            return false;
        owner[i] = ch;
    }
    return true;
}

static bool frame_valid(const struct tray_slot *slot, unsigned frame)
{
    return frame == ROCK_TRAY_HIDDEN || frame < slot->count;
}

static uint8_t selected_frame(const struct tray_slot *slot, uint32_t now)
{
    uint8_t frame = slot->frame;
    if (slot->steps && now >= slot->start)
    {
        uint32_t elapsed = now - slot->start;
        for (unsigned i = 0; i < slot->steps && elapsed >= slot->delays[i]; ++i)
            frame = slot->targets[i];
    }
    return frame;
}

static bool schedule_valid(const struct tray_slot *slot)
{
    if (slot->steps > ROCK_TRAY_MAX_RULES || (slot->steps && !slot->start))
        return false;

    uint32_t prior = 0;
    for (unsigned i = 0; i < slot->steps; ++i)
    {
        uint32_t delay = slot->delays[i];
        if (delay < prior || delay - prior < ROCK_TRAY_MIN_DELAY ||
            !frame_valid(slot, slot->targets[i]))
            return false;
        prior = delay;
    }
    return true;
}

static uint32_t checksum(const struct tray_store *data)
{
    return crc_32(data, offsetof(struct tray_store, crc), 0xffffffffu);
}

static bool store_valid(const struct tray_store *data)
{
    if (memcmp(data->magic, signature, sizeof(signature)) ||
        data->crc != checksum(data))
        return false;
    for (unsigned i = 0; i < TRAY_SLOTS; ++i)
    {
        const struct tray_slot *slot = &data->slots[i];
        if (!slot->owner[0])
            continue;
        size_t len = 0;
        while (len < sizeof(slot->owner) && owner_valid(slot->owner[len]))
            ++len;
        if (len == sizeof(slot->owner) || slot->owner[len] ||
            !slot->count || slot->count > ROCK_TRAY_MAX_FRAMES ||
            !frame_valid(slot, slot->frame) || !schedule_valid(slot))
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(slot->owner, data->slots[j].owner))
                return false;
    }
    return true;
}

static void widen(const struct rock_tray_bitmap *in, unsigned count,
                  struct rock_tray_icon12 *out)
{
    const int pad = (ROCK_TRAY_ICON12_SIZE - ROCK_TRAY_ICON_SIZE) / 2;
    memset(out, 0, count * sizeof(*out));
    for (unsigned i = 0; i < count; ++i)
        for (int c = 0; c < ROCK_TRAY_ICON_SIZE; ++c)
            out[i].columns[pad + c] = in[i].columns[c] << pad;
}

/* Widen a V1 file read into `store`, if it is one. */
static bool upgrade_v1(ssize_t length)
{
    struct tray_store_v1 old;
    if (length != sizeof(old))
        return false;
    memcpy(&old, &store, sizeof(old));
    if (memcmp(old.magic, signature_v1, sizeof(signature_v1)) ||
        old.crc != crc_32(&old, offsetof(struct tray_store_v1, crc),
                          0xffffffffu))
        return false;

    memset(&store, 0, sizeof(store));
    memcpy(store.magic, signature, sizeof(signature));
    for (unsigned i = 0; i < TRAY_SLOTS; ++i)
    {
        const struct tray_slot_v1 *from = &old.slots[i];
        struct tray_slot *to = &store.slots[i];
        memcpy(to->owner, from->owner, sizeof(to->owner));
        to->start = from->start;
        memcpy(to->delays, from->delays, sizeof(to->delays));
        widen(from->frames, ROCK_TRAY_MAX_FRAMES, to->frames);
        memcpy(to->targets, from->targets, sizeof(to->targets));
        to->count = from->count;
        to->frame = from->frame;
        to->steps = from->steps;
    }
    store.crc = checksum(&store);
    return true;
}

void app_tray_init(void)
{
    mutex_init(&store_lock);
    memset(&store, 0, sizeof(store));
    int fd = open(TRAY_PATH, O_RDONLY);
    if (fd < 0)
    {
        ready = errno == ENOENT;
        return;
    }

    ssize_t length = read(fd, &store, sizeof(store));
    char extra;
    ssize_t tail = length == sizeof(store) ? read(fd, &extra, 1) : 0;
    int closed = close(fd);
    ready = length >= 0 && tail >= 0 && closed == 0;
    bool loaded = ready && !tail &&
                  (length == sizeof(store) || upgrade_v1(length));
    if (!loaded || !store_valid(&store))
        memset(&store, 0, sizeof(store));
}

static bool save_store(struct tray_store *next)
{
    if (!memcmp(next->slots, store.slots, sizeof(store.slots)))
        return true;

    memcpy(next->magic, signature, sizeof(signature));
    next->crc = checksum(next);
    int fd = open(TRAY_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write(fd, next, sizeof(*next)) == sizeof(*next) && fsync(fd) == 0;
    if (close(fd) < 0)
        ok = false;

    if (!ok || rename(TRAY_TEMP, TRAY_PATH) < 0)
    {
        remove(TRAY_TEMP);
        return false;
    }
    mutex_lock(&store_lock);
    store = *next;
    mutex_unlock(&store_lock);
    return true;
}

static struct tray_slot *find_slot(struct tray_store *data, const char *owner,
                                   bool allocate)
{
    struct tray_slot *empty = NULL;
    for (unsigned i = 0; i < TRAY_SLOTS; ++i)
    {
        struct tray_slot *slot = &data->slots[i];
        if (!strcmp(slot->owner, owner))
            return slot;
        if (!slot->owner[0] && !empty)
            empty = slot;
    }
    return allocate ? empty : NULL;
}

bool app_tray_register(const char *path, const struct rock_tray_icon12 *frames,
                       unsigned count)
{
    char owner[TRAY_OWNER_SIZE];
    if (!ready || !owner_from_path(path, owner) ||
        count > ROCK_TRAY_MAX_FRAMES || (count && !frames))
        return false;

    struct tray_store next = store;
    struct tray_slot *slot = find_slot(&next, owner, count != 0);
    if (!slot)
        return count == 0;
    if (!count)
        memset(slot, 0, sizeof(*slot));
    else
    {
        if (!slot->owner[0])
        {
            memset(slot, 0, sizeof(*slot));
            memcpy(slot->owner, owner, sizeof(slot->owner));
            slot->frame = ROCK_TRAY_HIDDEN;
        }
        slot->count = count;
        if (!frame_valid(slot, slot->frame) || !schedule_valid(slot))
            return false;

        memset(slot->frames, 0, sizeof(slot->frames));
        for (unsigned i = 0; i < count; ++i)
            for (int c = 0; c < ROCK_TRAY_ICON12_SIZE; ++c)
                slot->frames[i].columns[c] = frames[i].columns[c] &
                    ((1u << ROCK_TRAY_ICON12_SIZE) - 1);
    }
    return save_store(&next);
}

bool app_tray_register8(const char *path, const struct rock_tray_bitmap *frames,
                        unsigned count)
{
    struct rock_tray_icon12 wide[ROCK_TRAY_MAX_FRAMES];
    if (count > ROCK_TRAY_MAX_FRAMES || (count && !frames))
        return false;
    widen(frames, count, wide);
    return app_tray_register(path, count ? wide : NULL, count);
}

bool app_tray_set(const char *path, unsigned frame)
{
    char owner[TRAY_OWNER_SIZE];
    if (!ready || !owner_from_path(path, owner))
        return false;
    struct tray_store next = store;
    struct tray_slot *slot = find_slot(&next, owner, false);
    if (!slot || !frame_valid(slot, frame))
        return false;
    slot->frame = frame;
    slot->steps = 0;
    slot->start = 0;
    memset(slot->delays, 0, sizeof(slot->delays));
    memset(slot->targets, 0, sizeof(slot->targets));
    return save_store(&next);
}

int app_tray_get(const char *path, const struct tm *now)
{
    char owner[TRAY_OWNER_SIZE];
    if (!ready || !owner_from_path(path, owner))
        return -1;

    const struct tray_slot *slot = find_slot(&store, owner, false);
    return slot ? selected_frame(slot, tray_clock(now)) : -1;
}

bool app_background_schedule(const char *path,
                             const struct rock_tray_rule *rules,
                             unsigned count, const struct tm *now)
{
    char owner[TRAY_OWNER_SIZE];
    if (!ready || !owner_from_path(path, owner) ||
        count > ROCK_TRAY_MAX_RULES || (count && !rules))
        return false;

    struct tray_store next = store;
    struct tray_slot *slot = find_slot(&next, owner, false);
    if (!slot)
        return false;
    uint32_t clock = tray_clock(now);
    slot->frame = selected_frame(slot, clock);
    slot->steps = count;
    slot->start = count ? clock : 0;
    memset(slot->delays, 0, sizeof(slot->delays));
    memset(slot->targets, 0, sizeof(slot->targets));
    for (unsigned i = 0; i < count; ++i)
    {
        slot->delays[i] = rules[i].delay_seconds;
        slot->targets[i] = rules[i].frame;
    }
    return schedule_valid(slot) && save_store(&next);
}

void app_tray_snapshot(const struct tm *now, struct app_tray_view *view)
{
    *view = (struct app_tray_view){0};
    if (!ready)
        return;

    mutex_lock(&store_lock);
    uint32_t clock = tray_clock(now);
    for (unsigned i = 0; i < TRAY_SLOTS && view->count < APP_TRAY_VISIBLE; ++i)
    {
        const struct tray_slot *slot = &store.slots[i];
        if (!slot->owner[0])
            continue;
        uint8_t frame = selected_frame(slot, clock);
        if (frame != ROCK_TRAY_HIDDEN)
            view->icons[view->count++] = slot->frames[frame];
    }
    mutex_unlock(&store_lock);
}
