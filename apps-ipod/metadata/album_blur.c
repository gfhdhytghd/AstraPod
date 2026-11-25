/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include <stdio.h>
#include "album_blur.h"
#include "album_palette.h"
#include "art_cache.h"
#include "art_name.h"
#include "audio.h"
#include "audio/buffering.h"
#include "audio/playback.h"
#include "button.h"
#include "system/appevents.h"
#include "core_alloc.h"
#include "crc32.h"
#include "debug.h"
#include "dir.h"
#include "draw/color.h"
#include "events.h"
#include "file.h"
#include "kernel.h"
#include "logfile.h"
#include "metadata.h"
#include "playlist/playlist.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "settings/settings.h"
#include "strlcpy.h"
#include "system.h"
#include "system/bg_task.h"
#include "system/debug_log.h"
#include "screens/covers/panel_perf.h"
#include "ui/native_theme_runtime.h"
#include "ui/native_wps.h"
#include "usb.h"

#define BLUR_DIR ROCKBOX_DIR "/podbox/cache/blur"
#define BLUR_MAGIC 0x344c4250u
#define BLUR_SLOTS 4
#define BLUR_EDGE 240
#define BLUR_W ((LCD_WIDTH + 3) / 4)
#define BLUR_H ((LCD_HEIGHT + 3) / 4)
#define BLUR_EVENT_WPS 0x1180
#define BLUR_EVENT_WINDOW BG_EVENT_BLUR
#define PANEL_ALPHA_MAX 224
/* How much darker the album panel's header band is than its rows. */
#define PANEL_BAND_ALPHA 80
#define WCAG_OFFSET 3277u /* 0.05 in Q16 */
#define STATUS_ART_GRACE HZ

struct blur_record {
    uint32_t magic, cover_hash, checksum;
    struct img_levels levels;
    uint16_t panel_alpha, wps_lum;
    fb_data plane[BLUR_W * BLUR_H];
};

struct blur_pixels {
    struct blur_record cached;
    fb_data panel[ALBUM_BLUR_PANEL_W * ALBUM_BLUR_PANEL_H];
};

static struct {
    struct album_blur_source source;
    unsigned hash, recent, users;
    long built;
    bool ready;
    uint8_t wps_lum;
    uint8_t tint_rows;
    unsigned tint;
} slots[BLUR_SLOTS];

/* The covers around Cover Flow's centre, centre first. A pass takes them in
 * order: `next` is the next to start and `done` the number finished.
 * `missing` marks those found to have no art; `rerun` starts another pass
 * when this one ends. */
static struct {
    struct album_blur_source source[ALBUM_BLUR_NEIGHBORS];
    unsigned count, next, done, revision;
    unsigned missing;
    bool rerun;
} window;
static struct album_blur_source wps_source;
static int wps_art = -1;
static unsigned wps_generation, wps_hash;
static int cache_handle, scratch_handle;
static unsigned use_clock;
static volatile unsigned cache_generation;
static struct mutex cache_lock;
static struct bg_task blur_task;
static volatile unsigned playing_revision;
static volatile long playing_changed_at;
static struct {
    unsigned revision, height, color;
    enum album_blur_tint_state state;
} status_tint;

/* Diagnostics: the cache's last steps, which Debug > Album Blur writes to
 * the background tasks log, and which go there as they happen while that
 * log is on. */
#define NOTE_RING 48
enum note_kind {
    NOTE_WINDOW, NOTE_AGAIN, NOTE_START, NOTE_RAM, NOTE_DISK, NOTE_COLD,
    NOTE_STALE, NOTE_NO_ART, NOTE_NO_SLOT, NOTE_EVICT, NOTE_MISS, NOTE_SHOWN,
    NOTE_USB,
};
static const char *const note_names[] = {
    "window", "again", "start", "ram", "disk", "cold", "stale", "no-art",
    "no-slot", "evict", "panel-miss", "panel-shown", "usb-has-disk",
};
/* Why the panel drew without its blur. */
enum miss_reason {
    MISS_NO_MEMORY, MISS_NO_KEY, MISS_UNREQUESTED, MISS_QUEUED,
    MISS_BUILDING, MISS_NO_ART, MISS_LOST,
};
static const char *const miss_names[] = {
    "no memory", "no art key", "not requested", "queued", "building",
    "no art", "lost",
};
static struct note {
    long tick;
    unsigned key, revision;
    int arg;
    int8_t slot;
    uint8_t kind;
    char type;
} notes[NOTE_RING];
static unsigned note_count;
/* The cover the panel is waiting for, while it waits. */
static struct {
    struct album_blur_source source;
    long since;
    int reason;
} panel_wait = { .reason = -1 };

static char source_type(const struct album_blur_source *source)
{
    if (source->remote[0])
        return 'r';
    if (!source->local)
        return '-';
    return source->portrait ? 'a' : 'l';
}

static unsigned source_key(const struct album_blur_source *source)
{
    return source->remote[0] ?
        (unsigned)crc_32(source->remote, strlen(source->remote), 0xffffffff) :
        source->local;
}

static void note_line(const struct note *n, char *line, size_t size)
{
    snprintf(line, size, "%s %c%08x slot %d rev %u arg %d",
             note_names[n->kind], n->type, n->key, n->slot, n->revision,
             n->arg);
}

static void note(enum note_kind kind, const struct album_blur_source *source,
                 int slot, int arg)
{
    struct note *n = &notes[note_count++ % NOTE_RING];
    n->tick = current_tick;
    n->kind = kind;
    n->type = source ? source_type(source) : '-';
    n->key = source ? source_key(source) : 0;
    n->slot = slot;
    n->revision = window.revision;
    n->arg = arg;
    char line[80];
    note_line(n, line, sizeof(line));
    DEBUGF("album blur: %s\n", line);
    debug_log(DEBUG_LOG_TASKS, "album blur: %s", line);
}

static bool same_source(const struct album_blur_source *a,
                        const struct album_blur_source *b)
{
    return a->local == b->local && a->portrait == b->portrait &&
           !strcmp(a->remote, b->remote);
}

static int find_slot(const struct album_blur_source *source)
{
    if (!source->local && !source->remote[0])
        return -1;
    for (int i = 0; i < BLUR_SLOTS; i++)
        if (slots[i].ready && same_source(source, &slots[i].source))
            return i;
    return -1;
}

static int acquire_slot(const struct album_blur_source *source)
{
    int slot = find_slot(source);
    if (slot >= 0)
    {
        slots[slot].recent = ++use_clock;
        slots[slot].users++;
    }
    return slot;
}

static void release_slot(int slot)
{
    mutex_lock(&cache_lock);
    slots[slot].users--;
    mutex_unlock(&cache_lock);
}

/* Whether a slot holds one of the window's covers. Call with the lock held. */
static bool in_window(int slot)
{
    for (unsigned i = 0; i < window.count; i++)
        if (same_source(&slots[slot].source, &window.source[i]))
            return true;
    return false;
}

static int reserve_slot(const struct album_blur_source *source, unsigned hash,
                        unsigned wps_lum, bool *hit)
{
    mutex_lock(&cache_lock);
    int slot = find_slot(source);
    *hit = slot >= 0 && slots[slot].hash == hash && slots[slot].wps_lum == wps_lum;
    if (*hit)
    {
        slots[slot].recent = ++use_clock;
        mutex_unlock(&cache_lock);
        return slot;
    }
    if (slot >= 0)
    {
        slots[slot].ready = false;
        ++cache_generation;
    }
    int victim = -1;
    for (int i = 0; i < BLUR_SLOTS; i++)
    {
        if (slots[i].users)
            continue;
        if (!slots[i].ready)
        {
            victim = i;
            break;
        }
        /* Slots outside the window go first, so Now Playing's art never
         * pushes out the blur behind the album panel. */
        bool kept = in_window(i);
        if (victim < 0 || in_window(victim) > kept ||
            (in_window(victim) == kept && slots[i].recent < slots[victim].recent))
            victim = i;
    }
    if (victim >= 0)
    {
        if (slots[victim].ready)
        {
            note(NOTE_EVICT, &slots[victim].source, victim, in_window(victim));
            ++cache_generation;
        }
        slots[victim].ready = false;
    }
    mutex_unlock(&cache_lock);
    return victim;
}

static void record_path(const struct album_blur_source *source, char out[MAX_PATH])
{
    if (source->remote[0])
        snprintf(out, MAX_PATH, BLUR_DIR "/r-%08x.blr",
                 (unsigned)crc_32(source->remote, strlen(source->remote), 0xffffffff));
    else
        snprintf(out, MAX_PATH, BLUR_DIR "/%c-%08x.blr",
                 source->portrait ? 'a' : 'l', source->local);
}

static uint32_t record_checksum(const struct blur_record *record)
{
    uint32_t crc = crc_32(&record->levels, sizeof(record->levels), 0xffffffff);
    crc = crc_32(&record->panel_alpha, sizeof(record->panel_alpha), crc);
    crc = crc_32(&record->wps_lum, sizeof(record->wps_lum), crc);
    return crc_32(record->plane, sizeof(record->plane), crc);
}

static bool read_record(const char *path, struct blur_record *record, unsigned hash)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, record, sizeof(*record)) == sizeof(*record) &&
              record->magic == BLUR_MAGIC && record->cover_hash == hash &&
              record->panel_alpha <= PANEL_ALPHA_MAX && record->wps_lum <= 255 &&
              record->checksum == record_checksum(record);
    close(fd);
    return ok;
}

static void write_record(const char *path, struct blur_record *record)
{
    if (usb_host_wants_storage())
        return;
    mkdir(ROCKBOX_DIR "/podbox");
    mkdir(ROCKBOX_DIR "/podbox/cache");
    mkdir(BLUR_DIR);
    char temp[MAX_PATH];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    record->checksum = record_checksum(record);
    bool complete = write(fd, record, sizeof(*record)) == sizeof(*record);
    if (close(fd) == 0 && complete && !usb_host_wants_storage() &&
        rename(temp, path) == 0)
        return;
    remove(temp);
}

static void columns_to_rows(fb_data *pixels, int dim)
{
    for (int y = 0; y < dim; y++)
        for (int x = 0; x < y; x++)
        {
            fb_data value = pixels[y * dim + x];
            pixels[y * dim + x] = pixels[x * dim + y];
            pixels[x * dim + y] = value;
        }
}

static int local_pixels(unsigned key, fb_data *pixels)
{
    int size = art_cache_size_index("wps");
    char path[MAX_PATH];
    bool fallback;
    struct bitmap bitmap = {
        .width = BLUR_EDGE, .height = BLUR_EDGE,
        .format = FORMAT_NATIVE, .data = (unsigned char *)pixels,
    };
    if (size >= 0 && art_cache_lookup_hash(key, size, path, sizeof(path), &fallback)
        && !fallback)
    {
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            int result = art_cache_load_aat(fd, &bitmap, FRAMEBUFFER_SIZE, NULL);
            close(fd);
            if (result > 0)
                return BLUR_EDGE;
        }
    }

    size = art_cache_size_index("coverflow");
    if (size < 0 || !art_cache_lookup_hash(key, size, path, sizeof(path), &fallback)
        || fallback)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct art_cache_header header;
    int dim = 0;
    if (read(fd, &header, sizeof(header)) == sizeof(header) &&
        header.magic == ART_CACHE_MAGIC && header.version == ART_CACHE_FORMAT_VERSION &&
        header.width == header.height && header.width > 0 && header.width <= BLUR_EDGE)
    {
        size_t bytes = header.width * header.height * sizeof(*pixels);
        if (read(fd, pixels, bytes) == (ssize_t)bytes)
        {
            dim = header.width;
            if (header.layout == AA_COLUMNS)
                columns_to_rows(pixels, dim);
            else if (header.layout != AA_ROWS)
                dim = 0;
        }
    }
    close(fd);
    return dim;
}

static bool copy_playing_art(fb_data *pixels, int art, int *width, int *height)
{
    if (art < 0 || !wps_art_pin(art, true))
        return false;
    struct bitmap *bitmap;
    bool got = wps_art_bitmap(art, &bitmap) && bitmap->data &&
               bitmap->width > 0 && bitmap->width <= BLUR_EDGE &&
               bitmap->height > 0 && bitmap->height <= BLUR_EDGE;
    if (got)
    {
        *width = bitmap->width;
        *height = bitmap->height;
        memcpy(pixels, bitmap->data, *width * *height * sizeof(*pixels));
    }
    wps_art_pin(art, false);
    return got;
}

static unsigned wps_ink_luminance(unsigned fg)
{
    return (RGB_UNPACK_RED(fg) * 77 + RGB_UNPACK_GREEN(fg) * 150 +
            RGB_UNPACK_BLUE(fg) * 29) >> 8;
}

static unsigned wps_scrim_luminance(const struct ui_theme *theme)
{
    if (theme->background[UI_BG_WPS] == UI_BG_SOLID)
        return 0;
    return wps_ink_luminance(ui_theme_rgb565(UI_COLOR_WPS_FG));
}

static void crop_panel(fb_data *panel, const fb_data *frame)
{
    const int crop = LCD_HEIGHT * ALBUM_BLUR_PANEL_W / ALBUM_BLUR_PANEL_H;
    for (int y = 0; y < ALBUM_BLUR_PANEL_H; y++)
    {
        const fb_data *row = frame + (y * LCD_HEIGHT / ALBUM_BLUR_PANEL_H) * LCD_WIDTH +
                             (LCD_WIDTH - crop) / 2;
        for (int x = 0; x < ALBUM_BLUR_PANEL_W; x++)
            panel[y * ALBUM_BLUR_PANEL_W + x] = row[x * crop / ALBUM_BLUR_PANEL_W];
    }
}

/* Now Playing uses a highlight curve, not a uniform alpha. Use its strongest
 * attenuation in the text regions as the floor for a single panel alpha.
 * Test the actual RGB565 output: quantisation can change which pixel is
 * brightest, so finding the minimum alpha from just one source pixel is unsafe. */
static unsigned panel_scrim(const fb_data *panel, const struct img_filter *wps)
{
    const unsigned ceiling = (color_srgb_luminance(ALBUM_BLUR_DIM_TEXT) +
                              WCAG_OFFSET) * 2 / 9 - WCAG_OFFSET;
    unsigned alpha = 0;
    for (int y = 1; y < ALBUM_BLUR_ROWS_TOP + ALBUM_BLUR_ROWS * ALBUM_BLUR_ROW_PITCH; y++)
    {
        if (y >= ALBUM_BLUR_DIVIDER && y < ALBUM_BLUR_ROWS_TOP)
            continue;
        const fb_data *row = panel + y * ALBUM_BLUR_PANEL_W;
        int right = y < ALBUM_BLUR_DIVIDER ? ALBUM_BLUR_HEADER_RIGHT : ALBUM_BLUR_ROWS_RIGHT;
        for (int x = ALBUM_BLUR_TEXT_LEFT; x < right; x++)
        {
            fb_data pixel = row[x];
            unsigned floor = 256 - img_filter_scrim_scale(wps, pixel);
            alpha = MAX(alpha, MIN(floor, PANEL_ALPHA_MAX));
            if (color_srgb_luminance(color_blend(pixel, LCD_BLACK, alpha)) > ceiling)
            {
                unsigned high = PANEL_ALPHA_MAX;
                while (alpha < high)
                {
                    unsigned mid = (alpha + high) / 2;
                    if (color_srgb_luminance(color_blend(pixel, LCD_BLACK, mid)) > ceiling)
                        alpha = mid + 1;
                    else
                        high = mid;
                }
            }
        }
    }
    return alpha;
}

static bool window_stale(unsigned revision)
{
    if (!revision)
        return false;
    mutex_lock(&cache_lock);
    bool stale = revision != window.revision;
    mutex_unlock(&cache_lock);
    return stale;
}

static unsigned prepare(const struct album_blur_source *source, int art,
                        unsigned wps_lum, unsigned revision, bool *missing)
{
    if (usb_host_wants_storage())
        return 0;
    if (!source->local && !source->remote[0])
    {
        if (missing)
            *missing = true;
        return 0;
    }
    unsigned result = 0;
    uint32_t start = USEC_TIMER;
    fb_data *scratch = core_get_data_pinned(scratch_handle);
    int width = 0, height = 0;
    if (!copy_playing_art(scratch, art, &width, &height))
    {
#ifdef HAVE_USB_PODLINK
        if (source->remote[0])
        {
            if (cider_copy_art(source->remote, "L", scratch, BLUR_EDGE * BLUR_EDGE * sizeof(*scratch)))
                width = height = BLUR_EDGE;
            else if (cider_copy_art(source->remote, "M", scratch, CIDER_COVER_SCRATCH_BYTES))
            {
                width = height = CIDER_COVER_DIM;
                columns_to_rows(scratch, width);
            }
            else if (cider_copy_art(source->remote, "XS", scratch, 32 * 32 * sizeof(*scratch)))
                width = height = 32;
        }
        else
#endif
            width = height = local_pixels(source->local, scratch);
    }
    if (window_stale(revision) || usb_host_wants_storage())
    {
        note(NOTE_STALE, source, -1, 0);
        goto done;
    }
    if (!width)
    {
        note(NOTE_NO_ART, source, -1, revision != 0);
        if (missing)
            *missing = true;
        mutex_lock(&cache_lock);
        int stale = find_slot(source);
        if (stale >= 0 && !source->remote[0])
        {
            slots[stale].ready = false;
            ++cache_generation;
        }
        mutex_unlock(&cache_lock);
        goto done;
    }

    unsigned hash = crc_32(scratch, width * height * sizeof(*scratch),
                           (width << 16) | height);
    if (!hash)
        hash = 1;
    bool hit;
    int slot = reserve_slot(source, hash, wps_lum, &hit);
    if (hit)
        DEBUGF("FU112_BLUR ram %08x %lu\n", hash, (unsigned long)(USEC_TIMER-start));
    if (slot < 0 || hit)
    {
        /* Now Playing asks every quarter second while it waits: only the
         * window's hits are worth a note. */
        if (!hit)
            note(NOTE_NO_SLOT, source, -1, revision != 0);
        else if (revision)
            note(NOTE_RAM, source, slot, (USEC_TIMER - start) / 1000);
        if (hit)
            panel_perf_record(PP_BLUR, USEC_TIMER - start, 2, hash);
        result = hit ? hash : 0;
        goto done;
    }

    struct blur_pixels *cache = core_get_data_pinned(cache_handle);
    struct blur_pixels *entry = &cache[slot];
    struct img_filter filter;
    img_filter_compile("blur12", IMG_CLASSES_ALL, &filter);
    char path[MAX_PATH];
    record_path(source, path);
    bool disk = read_record(path, &entry->cached, hash);
    if (!disk)
    {
        int crop_w = width, crop_h = height;
        if (width * LCD_HEIGHT > height * LCD_WIDTH)
            crop_w = MAX(1, height * LCD_WIDTH / LCD_HEIGHT);
        else
            crop_h = MAX(1, width * LCD_HEIGHT / LCD_WIDTH);
        entry->cached.magic = BLUR_MAGIC;
        entry->cached.cover_hash = hash;
        img_filter_measure(scratch, width, height, &entry->cached.levels);
        img_filter_prepare_blur(entry->cached.plane, ARRAYLEN(entry->cached.plane),
                                LCD_WIDTH, LCD_HEIGHT,
                                scratch + (height - crop_h) / 2 * width + (width - crop_w) / 2,
                                crop_w, crop_h, width, &filter);
    }
    img_filter_render_prepared(scratch, LCD_WIDTH, LCD_HEIGHT, entry->cached.plane, &filter);
    crop_panel(entry->panel, scratch);
    if (!disk || entry->cached.wps_lum != wps_lum)
    {
        img_filter_compile("blur12+scrim+dither", IMG_CLASSES_ALL, &filter);
        img_filter_adapt(&filter, &entry->cached.levels, wps_lum);
        entry->cached.panel_alpha = panel_scrim(entry->panel, &filter);
        entry->cached.wps_lum = wps_lum;
        if (!window_stale(revision) && !usb_host_wants_storage())
            write_record(path, &entry->cached);
    }
    unsigned scrim = entry->cached.panel_alpha;
    if (scrim)
        for (unsigned i = 0; i < ARRAYLEN(entry->panel); i++)
            entry->panel[i] = color_blend(entry->panel[i], LCD_BLACK, scrim);
    for (unsigned i = 0; i < ALBUM_BLUR_DIVIDER * ALBUM_BLUR_PANEL_W; i++)
        entry->panel[i] = color_blend(entry->panel[i], LCD_BLACK, PANEL_BAND_ALPHA);
    DEBUGF("FU112_BLUR %s %08x %lu %u\n", disk ? "disk" : "cold", hash,
           (unsigned long)(USEC_TIMER-start), scrim);
    panel_perf_record(PP_BLUR, USEC_TIMER - start, disk, hash);
    core_put_data_pinned(cache);
    mutex_lock(&cache_lock);
    if (revision && revision != window.revision)
    {
        mutex_unlock(&cache_lock);
        note(NOTE_STALE, source, slot, 1);
        goto done;
    }
    slots[slot].built = current_tick;
    slots[slot].source = *source;
    slots[slot].hash = hash;
    slots[slot].wps_lum = wps_lum;
    slots[slot].recent = ++use_clock;
    slots[slot].tint_rows = 0;
    slots[slot].ready = true;
    ++cache_generation;
    mutex_unlock(&cache_lock);
    note(disk ? NOTE_DISK : NOTE_COLD, source, slot,
         (USEC_TIMER - start) / 1000);
    result = hash;

done:
    core_put_data_pinned(scratch);
    return result;
}

/* Only the shared worker writes the plane or its memoized status colour. */
static bool cover_tint(const struct album_blur_source *source, unsigned hash,
                       unsigned height, unsigned *tint)
{
    mutex_lock(&cache_lock);
    int slot = find_slot(source);
    bool valid = slot >= 0 && slots[slot].hash == hash;
    mutex_unlock(&cache_lock);
    if (!valid)
        return false;

    unsigned rows = (height * BLUR_H + LCD_HEIGHT - 1) / LCD_HEIGHT;
    if (slots[slot].tint_rows != rows)
    {
        struct blur_pixels *cache = core_get_data_pinned(cache_handle);
        slots[slot].tint = color_srgb_mean(cache[slot].cached.plane, rows * BLUR_W);
        slots[slot].tint_rows = rows;
        core_put_data_pinned(cache);
    }
    *tint = slots[slot].tint;
    return true;
}

static void publish_status_tint(unsigned revision, unsigned generation,
                                unsigned height, enum album_blur_tint_state state,
                                unsigned tint)
{
    if (state == ALBUM_BLUR_WAITING)
        return;
    int level = disable_irq_save();
    bool current = revision == playing_revision && generation == album_art_generation();
    bool changed = status_tint.state != state || status_tint.color != tint ||
                   status_tint.height != height || status_tint.revision != revision;
    if (current && changed)
    {
        status_tint.state = state;
        status_tint.color = tint;
        status_tint.height = height;
        status_tint.revision = revision;
    }
    restore_irq(level);
    if (current && changed && lcd_active() && button_queue_empty())
        button_queue_post(BUTTON_NONE, 0);
}

static void blur_work(const struct queue_event *ev)
{
    if (usb_host_wants_storage())
    {
        if (ev->id == BLUR_EVENT_WINDOW)
            note(NOTE_USB, NULL, -1, 0);
        return;
    }
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    unsigned wps_lum = wps_scrim_luminance(&theme);
    if (ev->id == BLUR_EVENT_WINDOW)
    {
        mutex_lock(&cache_lock);
        unsigned revision = window.revision;
        if (window.next >= window.count)
        {
            mutex_unlock(&cache_lock);
            return;
        }
        unsigned index = window.next++;
        struct album_blur_source source = window.source[index];
        mutex_unlock(&cache_lock);
        note(NOTE_START, &source, -1, index);
        bool missing = false;
        prepare(&source, -1, wps_lum, revision, &missing);
        mutex_lock(&cache_lock);
        bool more = revision == window.revision;
        if (more)
        {
            window.done = index + 1;
            if (missing)
                window.missing |= 1u << index;
        }
        if (more && window.next >= window.count)
        {
            more = window.rerun;
            window.rerun = false;
            if (more)
                window.next = window.done = 0;
        }
        mutex_unlock(&cache_lock);
        if (more && !bg_task_queued(&blur_task, BLUR_EVENT_WINDOW))
            bg_task_post(&blur_task, BLUR_EVENT_WINDOW);
        return;
    }

    /* The covers around Cover Flow's centre go first: their panel is what
     * is on screen. */
    mutex_lock(&cache_lock);
    bool window_waits = window.next < window.count;
    mutex_unlock(&cache_lock);
    if (window_waits)
    {
        if (!bg_task_queued(&blur_task, BLUR_EVENT_WINDOW))
            bg_task_post(&blur_task, BLUR_EVENT_WINDOW);
        if (!bg_task_queued(&blur_task, BLUR_EVENT_WPS))
            bg_task_post(&blur_task, BLUR_EVENT_WPS);
        return;
    }

    unsigned revision = playing_revision;
    bool remote = cider_active();
    int art = album_art_current();
    unsigned generation = album_art_generation();
    bool tint_enabled = global_settings.status_cover_tint &&
                        theme.background[UI_BG_STATUS] == UI_BG_COVER_TINT;
    int source_art = art;
    int track_slot = !remote && tint_enabled ? native_wps_art_slot() : -1;
    if (track_slot >= 0)
    {
        /* Album art for WPS can hold the old handle while this track buffers. */
        source_art = playback_track_aa_hid(track_slot);
        if (source_art < 0 && source_art != ERR_UNSUPPORTED_TYPE)
            return;
    }
    struct album_blur_source source = {0};
#ifdef HAVE_USB_PODLINK
    if (remote)
        cider_current_art(source.remote);
    else
#endif
    {
        const struct mp3entry *track = audio_status() ? audio_current_track() : NULL;
        if (track && track->path[0])
        {
            char path[MAX_PATH];
            strlcpy(path, track->path, sizeof(path));
            bool auto_portrait = global_settings.wps_art_source == WPS_ART_AUTO &&
                                 playlist_is_from_artist();
            if (global_settings.wps_art_source == WPS_ART_ARTIST || auto_portrait)
            {
                const char *artist = track->albumartist && track->albumartist[0]
                                   ? track->albumartist : track->artist;
                unsigned key = art_name_hash(artist);
                int size = art_cache_size_index("wps");
                char portrait_path[MAX_PATH];
                bool fallback;
                if (key && size >= 0 &&
                    art_cache_lookup_hash(key, size, portrait_path, sizeof(portrait_path),
                                          &fallback) && !fallback)
                {
                    source.portrait = true;
                    source.local = key;
                }
            }
            if (!source.portrait && global_settings.wps_art_source != WPS_ART_ARTIST)
                source.local = art_cache_track_key(path);
        }
    }
    /* A remote WPS bitmap may still belong to the previous cover key. */
#ifdef HAVE_USB_PODLINK
    if (remote && strcmp(source.remote, cider_wps_art_key()))
        source_art = -1;
#endif
    bool missing = false;
    unsigned hash = prepare(&source, source_art, wps_lum, 0, &missing);
    if (generation != album_art_generation() || art != album_art_current() ||
        revision != playing_revision || remote != cider_active() ||
        (track_slot >= 0 && source_art != playback_track_aa_hid(track_slot)))
        return;
#ifdef HAVE_USB_PODLINK
    if (remote)
    {
        char key[sizeof(source.remote)];
        cider_current_art(key);
        if (strcmp(key, source.remote))
            return;
    }
#endif
    mutex_lock(&cache_lock);
    if (wps_art != art || wps_generation != generation || wps_hash != hash ||
        !same_source(&wps_source, &source))
    {
        wps_source = source;
        wps_art = art;
        wps_generation = generation;
        wps_hash = hash;
        ++cache_generation;
    }
    mutex_unlock(&cache_lock);

    if (!tint_enabled)
        return;

    unsigned height = theme.metric[UI_METRIC_STATUS_HEIGHT];
    unsigned tint = 0;
    enum album_blur_tint_state state = ALBUM_BLUR_WAITING;
#ifdef HAVE_USB_PODLINK
    /* The same remote image can arrive in several sizes during playback. */
    static char tinted_key[sizeof(source.remote)];
    static unsigned tinted_height, remote_tint;
    if (source.remote[0] && !strcmp(source.remote, tinted_key) &&
        height == tinted_height)
    {
        state = ALBUM_BLUR_TINT;
        tint = remote_tint;
    }
    else if (source.remote[0])
    {
        if (hash && cover_tint(&source, hash, height, &tint))
        {
            state = ALBUM_BLUR_TINT;
            strlcpy(tinted_key, source.remote, sizeof(tinted_key));
            tinted_height = height;
            remote_tint = tint;
        }
        else if (cider_request_playing_art(source.remote))
            state = ALBUM_BLUR_NO_ART;
    }
    else
#endif
    {
        if (hash && cover_tint(&source, hash, height, &tint))
            state = ALBUM_BLUR_TINT;
        else if (missing && (!remote ||
                 TIME_AFTER(current_tick, playing_changed_at + STATUS_ART_GRACE)))
            state = ALBUM_BLUR_NO_ART;
    }

    publish_status_tint(revision, generation, height, state, tint);
}

static struct bg_task blur_task = { .name = "album blur", .handle_event = blur_work };

void album_blur_alloc(void)
{
    cache_handle = core_alloc_free_space(BLUR_SLOTS * sizeof(struct blur_pixels));
    scratch_handle = core_alloc_free_space(FRAMEBUFFER_SIZE);
    if (cache_handle > 0 && scratch_handle <= 0)
        cache_handle = core_free(cache_handle);
}

static void track_changed(unsigned short id, void *data)
{
    (void)id;
    (void)data;
    album_blur_playing_changed();
}

void album_blur_init(void)
{
    mutex_init(&cache_lock);
    if (cache_handle > 0)
    {
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
        add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_changed);
    }
}

void album_blur_request_wps(void)
{
    if (cache_handle > 0 && !bg_task_queued(&blur_task, BLUR_EVENT_WPS))
        bg_task_post(&blur_task, BLUR_EVENT_WPS);
}

void album_blur_playing_changed(void)
{
    int level = disable_irq_save();
    ++playing_revision;
    playing_changed_at = current_tick;
    restore_irq(level);
    album_blur_request_wps();
}

enum album_blur_tint_state album_blur_status_tint(unsigned height, unsigned *color)
{
    if (cache_handle <= 0)
        return ALBUM_BLUR_NO_ART;

    int level = disable_irq_save();
    enum album_blur_tint_state state = ALBUM_BLUR_WAITING;
    if (status_tint.revision == playing_revision && status_tint.height == height)
    {
        state = status_tint.state;
        *color = status_tint.color;
    }
    restore_irq(level);

    /* A cover may still be buffering, or its cache slot may have been pinned. */
    static long next_retry;
    if (state == ALBUM_BLUR_WAITING && !TIME_BEFORE(current_tick, next_retry))
    {
        next_retry = current_tick + HZ / 4;
        album_blur_request_wps();
    }
    return state;
}

/* Another pass over the window, after the one under way. Call with the lock
 * held; true when the caller must post the event. */
static bool look_again(void)
{
    window.missing = 0;
    if (window.next < window.count || window.done < window.next)
    {
        window.rerun = true;
        return false;
    }
    window.next = window.done = 0;
    return true;
}

void album_blur_request_window(const struct album_blur_source *sources, unsigned count)
{
    if (cache_handle <= 0)
        return;
    count = MIN(count, ALBUM_BLUR_NEIGHBORS);
    mutex_lock(&cache_lock);
    bool same = count && count == window.count;
    for (unsigned i = 0; same && i < count; i++)
        same = same_source(&sources[i], &window.source[i]);
    /* The same covers asked for again, as a slide or a cached cover arrives:
     * a new revision would throw away the blur being built for the centre. */
    bool post = same ? look_again() : count > 0;
    if (!same)
    {
        window.count = count;
        window.next = window.done = window.missing = 0;
        window.rerun = false;
        if (++window.revision == 0)
            ++window.revision;
        if (count)
            memcpy(window.source, sources, count * sizeof(*sources));
    }
    mutex_unlock(&cache_lock);
    /* A cover arriving asks again often: only a pass started anew is noted. */
    if (!same)
        note(NOTE_WINDOW, count ? &sources[0] : NULL, -1, count);
    else if (post)
        note(NOTE_AGAIN, &sources[0], -1, count);
    if (post && !bg_task_queued(&blur_task, BLUR_EVENT_WINDOW))
        bg_task_post(&blur_task, BLUR_EVENT_WINDOW);
}

unsigned album_blur_generation(void)
{
    return cache_generation;
}

static void write_state(int fd);

/* Notes the start and end of a wait for the panel's blur. */
static void panel_waits(const struct album_blur_source *source, int reason)
{
    bool same = panel_wait.reason >= 0 &&
                same_source(source, &panel_wait.source);
    if (reason < 0)
    {
        if (same)
            note(NOTE_SHOWN, source, -1,
                 (current_tick - panel_wait.since) * 1000 / HZ);
        panel_wait.reason = -1;
        return;
    }
    if (same && reason == panel_wait.reason)
        return;
    if (!same)
    {
        panel_wait.source = *source;
        panel_wait.since = current_tick;
    }
    panel_wait.reason = reason;
    note(NOTE_MISS, source, -1, reason);
    if (debug_log_enabled(DEBUG_LOG_TASKS))
    {
        /* Puts the lines held so far above the state. */
        debug_log_sync(DEBUG_LOG_TASKS, "album blur: the panel waits");
        int fd = debug_log_open(DEBUG_LOG_TASKS);
        if (fd >= 0)
        {
            write_state(fd);
            debug_log_close(DEBUG_LOG_TASKS, fd);
        }
    }
}

bool album_blur_draw_panel(const struct album_blur_source *source, int top, int height)
{
    if (cache_handle <= 0)
    {
        panel_waits(source, MISS_NO_MEMORY);
        return false;
    }
    mutex_lock(&cache_lock);
    int slot = acquire_slot(source);
    bool post = false;
    int reason = slot >= 0 ? -1 :
                 !source->local && !source->remote[0] ? MISS_NO_KEY :
                 MISS_UNREQUESTED;
    for (unsigned i = 0; slot < 0 && i < window.count; i++)
    {
        if (!same_source(source, &window.source[i]))
            continue;
        if (window.missing & 1u << i)
            reason = MISS_NO_ART;
        else if (i < window.done)
        {
            /* Its blur has gone since its pass: built again, and the panel
             * redrawn when it is ready. */
            reason = MISS_LOST;
            post = look_again();
        }
        else
            reason = i < window.next ? MISS_BUILDING : MISS_QUEUED;
        break;
    }
    mutex_unlock(&cache_lock);
    if (post && !bg_task_queued(&blur_task, BLUR_EVENT_WINDOW))
        bg_task_post(&blur_task, BLUR_EVENT_WINDOW);
    panel_waits(source, reason);
    if (slot < 0)
        return false;
    struct blur_pixels *cache = core_get_data_pinned(cache_handle);
    lcd_bitmap_part(cache[slot].panel, 0, top, ALBUM_BLUR_PANEL_W, 0, top,
                    ALBUM_BLUR_PANEL_W, height);
    core_put_data_pinned(cache);
    release_slot(slot);
    return true;
}

static bool render_wps_slot(fb_data *out, int slot, unsigned fg)
{
    if (slot < 0)
        return false;
    struct blur_pixels *cache = core_get_data_pinned(cache_handle);
    struct img_filter filter;
    img_filter_compile("blur12+scrim+dither", IMG_CLASSES_ALL, &filter);
    img_filter_adapt(&filter, &cache[slot].cached.levels, wps_ink_luminance(fg));
    img_filter_render_prepared(out, LCD_WIDTH, LCD_HEIGHT,
                               cache[slot].cached.plane, &filter);
    core_put_data_pinned(cache);
    release_slot(slot);
    return true;
}

bool album_blur_render_cover_wps(fb_data *out,
                                 const struct album_blur_source *source,
                                 unsigned fg)
{
    if (cache_handle <= 0)
        return false;
    mutex_lock(&cache_lock);
    int slot = acquire_slot(source);
    mutex_unlock(&cache_lock);
    return render_wps_slot(out, slot, fg);
}

bool album_blur_render_wps(fb_data *out, int art, unsigned generation, unsigned fg)
{
    mutex_lock(&cache_lock);
    int slot = art == wps_art && generation == wps_generation ? find_slot(&wps_source) : -1;
    if (slot >= 0 && slots[slot].hash == wps_hash)
    {
        slots[slot].recent = ++use_clock;
        slots[slot].users++;
    }
    else
        slot = -1;
    mutex_unlock(&cache_lock);
    return render_wps_slot(out, slot, fg);
}

static void write_source(int fd, const struct album_blur_source *source)
{
    logfile_printf(fd, "%c%08x", source_type(source), source_key(source));
}

/* The window, the slots and the panel's wait. */
static void write_state(int fd)
{
    mutex_lock(&cache_lock);
    logfile_printf(fd, "album blur at %ld: memory %s, generation %u%s\n",
                   current_tick, cache_handle > 0 ? "yes" : "NO",
                   cache_generation,
                   usb_host_wants_storage() ? ", USB host has the disk" : "");
    logfile_printf(fd, "  window rev %u count %u next %u done %u missing %x%s%s\n",
                   window.revision, window.count, window.next, window.done,
                   window.missing, window.rerun ? " rerun" : "",
                   bg_task_queued(&blur_task, BLUR_EVENT_WINDOW) ?
                   " queued" : "");
    for (unsigned i = 0; i < window.count; i++)
    {
        logfile_printf(fd, "    [%u] ", i);
        write_source(fd, &window.source[i]);
        logfile_printf(fd, " slot %d\n", find_slot(&window.source[i]));
    }
    /* The darkening is up to PANEL_ALPHA_MAX of 256: near it, a bright
     * cover's blur is close to black. */
    struct blur_pixels *cache = cache_handle > 0 ?
                                core_get_data_pinned(cache_handle) : NULL;
    for (int i = 0; i < BLUR_SLOTS; i++)
    {
        logfile_printf(fd, "  slot %d %s ", i,
                       slots[i].ready ? "ready" : "empty");
        write_source(fd, &slots[i].source);
        logfile_printf(fd, " hash %08x darkening %u users %u recent %u"
                       " built %ld ms ago%s\n",
                       slots[i].hash, cache ? cache[i].cached.panel_alpha : 0,
                       slots[i].users, slots[i].recent,
                       (current_tick - slots[i].built) * 1000 / HZ,
                       in_window(i) ? " window" : "");
    }
    if (cache)
        core_put_data_pinned(cache);
    logfile_printf(fd, "  now playing ");
    write_source(fd, &wps_source);
    logfile_printf(fd, " hash %08x\n", wps_hash);
    if (panel_wait.reason >= 0)
    {
        logfile_printf(fd, "  panel waits for ");
        write_source(fd, &panel_wait.source);
        logfile_printf(fd, ": %s, %ld ms\n", miss_names[panel_wait.reason],
                       (current_tick - panel_wait.since) * 1000 / HZ);
    }
    mutex_unlock(&cache_lock);
}

void album_blur_write_diagnostics(int fd)
{
    char line[80];
    write_state(fd);
    unsigned count = note_count;
    unsigned first = count > NOTE_RING ? count - NOTE_RING : 0;
    for (unsigned i = first; i < count; i++)
    {
        const struct note *n = &notes[i % NOTE_RING];
        note_line(n, line, sizeof(line));
        logfile_printf(fd, "  %ld ms: %s\n",
                       (n->tick - current_tick) * 1000 / HZ, line);
    }
}
