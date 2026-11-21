/* Pixel-native PodBox bitmap font reader. GPL v2+. */
#include "font_pbf.h"
#include "file.h"
#include "crc32.h"
#include "core_alloc.h"
#include "system.h"
#include "diacritic.h"
#include "kernel.h"
#include "string-extra.h"
#include <stdlib.h>
#include <string.h>

enum {
    HEADER_BYTES = 36,
    PAGE_ENTRIES = 64,
    INDEX_BYTES = 14,
    CACHE_BUDGET = 448 * 1024,
    PREFETCH_GLYPHS = 384,
    MAX_RECORD = 16384,
    RECORD_RGB = 0x80,
    BLOCK_BYTES = 4096,
    FIXED_TRAILER = 32,
    HAN_COUNT = 0xa000 - 0x3400,
    HANGUL_COUNT = 0xd7a4 - 0xac00,
    RANK_BYTES = (HAN_COUNT + HANGUL_COUNT + 0xfb00 - 0xf900) * 2,
    HOT_RESERVE = 8 * 1024 * 1024,
    /* Unaligned word reads in the LCD alpha blitter can read ahead. */
    ALPHA_PADDING = 8,
};

static unsigned u16(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16;
}

static size_t resident_bytes(const struct font *font)
{
    return font->pbf.index_offset - HEADER_BYTES;
}

static size_t map_bytes(const struct font_pbf *file)
{
    return file->stride ? RANK_BYTES + file->variable_count * INDEX_BYTES : 0;
}

static size_t map_position(const struct font *font)
{
    return resident_bytes(font) + PAGE_ENTRIES * INDEX_BYTES;
}

static size_t cache_offset(const struct font *font)
{
    return (map_position(font) + map_bytes(&font->pbf) + 3) & ~3u;
}

bool font_pbf_header(int fd, struct font *font)
{
    unsigned char head[HEADER_BYTES];
    if (read(fd, head, sizeof(head)) != sizeof(head) || memcmp(head, "PBF1", 4))
        return false;

    memset(font, 0, sizeof(*font));
    font->height = u16(head + 4);
    font->ascent = u16(head + 6);
    font->maxwidth = u16(head + 8);
    font->depth = 1;
    font->pbf.max_record = u16(head + 10);
    font->pbf.anchors = u16(head + 12);
    font->defaultchar = 0xfffd;
    font->size = u32(head + 16);
    uint32_t rows = u32(head + 20);
    font->pbf.index_offset = u32(head + 24);
    font->pbf.bitmap_offset = u32(head + 28);
    font->pbf.file_size = u32(head + 32);
    if (!font->height || font->height > 127 || font->ascent > (int)font->height ||
        !font->maxwidth || font->maxwidth > 127 ||
        font->size < 1 || font->size > 65535 || u16(head + 14) >= (unsigned)font->size ||
        font->pbf.max_record < 5 || font->pbf.max_record > MAX_RECORD ||
        font->pbf.anchors != (font->size + PAGE_ENTRIES - 1) / PAGE_ENTRIES ||
        !rows || rows > 256 ||
        font->pbf.index_offset < HEADER_BYTES + font->pbf.anchors * 4 + (rows + 1) * 2 ||
        font->pbf.index_offset > HEADER_BYTES + 96 * 1024 ||
        font->pbf.bitmap_offset != font->pbf.index_offset + (unsigned)font->size * INDEX_BYTES ||
        font->pbf.file_size <= font->pbf.bitmap_offset || (off_t)font->pbf.file_size != ffilesize(fd))
        return false;

    /* Older PBF1 files predate the optional ink bounds trailer. */
    font->pbf.ink_above = MAX(3u, (font->height + 5) / 6);
    font->pbf.ink_below = MAX(5u, (font->height + 3) / 4);
    unsigned char bounds[6];
    off_t end = font->pbf.file_size - sizeof(bounds);
    if (lseek(fd, end, SEEK_SET) == end && read(fd, bounds, sizeof(bounds)) == sizeof(bounds) &&
        !memcmp(bounds, "INK1", 4) && font->height + bounds[4] + bounds[5] <= 160)
    {
        font->pbf.ink_above = bounds[4];
        font->pbf.ink_below = bounds[5];
    }
    unsigned char fixed[FIXED_TRAILER];
    end -= sizeof(fixed);
    if (end >= (off_t)font->pbf.bitmap_offset && lseek(fd, end, SEEK_SET) == end &&
        read(fd, fixed, sizeof(fixed)) == sizeof(fixed) && !memcmp(fixed, "FIX1", 4))
    {
        uint32_t map = u32(fixed + 4), hot = u32(fixed + 8), cold = u32(fixed + 12);
        uint32_t stride = u32(fixed + 16), common = u32(fixed + 20);
        uint32_t count = u32(fixed + 24), variable = u32(fixed + 28);
        if (map < font->pbf.bitmap_offset || map > hot || hot > cold ||
            cold > (uint32_t)end || (map | hot | cold) % BLOCK_BYTES ||
            !common || common > count || count > (unsigned)font->size ||
            variable != (unsigned)font->size - count || !variable ||
            stride < 6 + 5 + ALPHA_PADDING || stride > MAX_RECORD + 16 ||
            RANK_BYTES + variable * INDEX_BYTES > hot - map ||
            common > (cold - hot) / stride || count - common > ((uint32_t)end - cold) / stride)
            return false;
        font->pbf.map_offset = map;
        font->pbf.hot_offset = hot;
        font->pbf.cold_offset = cold;
        font->pbf.stride = stride;
        font->pbf.hot_count = common;
        font->pbf.fixed_count = count;
        font->pbf.variable_count = variable;
    }
    font->pbf.rows = rows;
    font->pbf.page = UINT32_MAX;
    return true;
}

static size_t cache_entry_size(const struct font *font)
{
    /* Six byte prefix: record size, advance, two kerning classes. */
    return ((sizeof(struct font_cache_entry) + 6 + font->pbf.max_record +
             ALPHA_PADDING + sizeof(ucschar_t) - 1) & ~(sizeof(ucschar_t) - 1))
           + LRU_SLOT_OVERHEAD + sizeof(short);
}

size_t font_pbf_buffer_size(const struct font *font, size_t limit, int glyphs)
{
    size_t entry = cache_entry_size(font);
    bool page_font = font->height <= 31;
    size_t cap = (page_font ? CACHE_BUDGET : 128 * 1024) / entry;
    if (limit)
        cap = MIN(cap, limit > cache_offset(font) ?
                         (limit - cache_offset(font)) / entry : 0);
    if (page_font && !limit)
        glyphs = MAX(glyphs, PREFETCH_GLYPHS);
    /* The existing LRU aligns its index for 32-bit Unicode. */
    size_t slots = MAX(2, MIN(cap, (unsigned)MAX(2, glyphs))) & ~1u;
    return cache_offset(font) + slots * entry;
}

bool font_pbf_open(struct font *font)
{
    static unsigned serial;
    unsigned char *buf = font->buffer_start;
    size_t bytes = resident_bytes(font);
    if (lseek(font->fd, HEADER_BYTES, SEEK_SET) != HEADER_BYTES ||
        read(font->fd, buf, bytes) != (ssize_t)bytes)
        return false;

    for (unsigned i = 1; i < font->pbf.anchors; ++i)
        if (u32(buf + i * 4) <= u32(buf + (i - 1) * 4))
            return false;
    const unsigned char *rows = buf + font->pbf.anchors * 4;
    size_t pairs = bytes - font->pbf.anchors * 4 - (font->pbf.rows + 1) * 2;
    if (u16(rows) != 0 || u16(rows + 2 * font->pbf.rows) * 3 != pairs)
        return false;
    for (unsigned i = 1; i <= font->pbf.rows; ++i)
        if (u16(rows + i * 2) < u16(rows + (i - 1) * 2))
            return false;

    size_t directory_bytes = map_bytes(&font->pbf);
    if (directory_bytes &&
        (lseek(font->fd, font->pbf.map_offset, SEEK_SET) != (off_t)font->pbf.map_offset ||
         read(font->fd, buf + map_position(font), directory_bytes) != (ssize_t)directory_bytes))
        return false;
    size_t offset = cache_offset(font);
    font_cache_create(&font->cache, buf + offset, font->buffer_size - offset,
                      6 + font->pbf.max_record + ALPHA_PADDING);
    font->pbf.serial = ++serial;
    return true;
}

static const unsigned char *glyph_index(struct font *font, uint32_t cp)
{
    const unsigned char *anchors = font->buffer_start;
    unsigned lo = 0, hi = font->pbf.anchors;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (u32(anchors + mid * 4) <= cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return NULL;

    unsigned page = lo - 1;
    unsigned count = MIN(PAGE_ENTRIES, (unsigned)font->size - page * PAGE_ENTRIES);
    unsigned char *index = font->buffer_start + resident_bytes(font);
    if (page != font->pbf.page)
    {
        size_t bytes = count * INDEX_BYTES;
        off_t offset = font->pbf.index_offset + page * PAGE_ENTRIES * INDEX_BYTES;
        font->pbf.page = UINT32_MAX;
        if (lseek(font->fd, offset, SEEK_SET) != offset ||
            read(font->fd, index, bytes) != (ssize_t)bytes)
            return NULL;
        font->pbf.page = page;
    }

    lo = 0;
    hi = count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        const unsigned char *record = index + mid * INDEX_BYTES;
        uint32_t code = u32(record);
        if (code == cp)
            return record;
        if (code < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* The codepoint selects a rank in RAM; the rank selects a fixed disk cell. */
static uint32_t fixed_offset(const struct font *font, uint32_t cp)
{
    if (!font->pbf.stride)
        return 0;
    unsigned ordinal;
    if (cp >= 0x3400 && cp < 0xa000)
        ordinal = cp - 0x3400;
    else if (cp >= 0xac00 && cp < 0xd7a4)
        ordinal = HAN_COUNT + cp - 0xac00;
    else if (cp >= 0xf900 && cp < 0xfb00)
        ordinal = HAN_COUNT + HANGUL_COUNT + cp - 0xf900;
    else
        return 0;
    unsigned rank = u16(font->buffer_start + map_position(font) + ordinal * 2);
    if (rank >= font->pbf.fixed_count)
        return 0;
    if (rank < font->pbf.hot_count)
        return font->pbf.hot_offset + rank * font->pbf.stride;
    return font->pbf.cold_offset + (rank - font->pbf.hot_count) * font->pbf.stride;
}

/* Reserve before playback claims the core heap. Lease one slot per lyric size
 * until its font and any in-flight background write have released it. */
static const struct { unsigned height; size_t capacity; } hot_slots[] = {
    {14, 896 * 1024}, {19, 1408 * 1024}, {21, 1536 * 1024},
};
static unsigned char *hot_memory;
static bool hot_used[ARRAYLEN(hot_slots)];

void font_pbf_reserve_hot(void)
{
    size_t total = 0;
    for (unsigned i = 0; i < ARRAYLEN(hot_slots); i++)
        total += hot_slots[i].capacity;
    if (!hot_memory && core_available() >= HOT_RESERVE + total)
    {
        int handle = core_alloc_free_space(total);
        if (handle > 0)
            hot_memory = core_get_data_pinned(handle);
    }
}

static unsigned char *hot_data(int slot)
{
    size_t offset = 0;
    for (int i = 1; i < slot; i++)
        offset += hot_slots[i - 1].capacity;
    return hot_memory + offset;
}

static size_t hot_bytes(const struct font_pbf *file)
{
    return (size_t)file->hot_count * file->stride;
}

static const unsigned char *hot_glyph(const struct font *font, uint32_t cp)
{
    if (!font->pbf.hot_ready)
        return NULL;
    uint32_t offset = fixed_offset(font, cp);
    if (offset < font->pbf.hot_offset || offset >= font->pbf.hot_offset + hot_bytes(&font->pbf))
        return NULL;
    const unsigned char *data = hot_data(font->pbf.hot_slot);
    return data + offset - font->pbf.hot_offset;
}

static const unsigned char *variable_index(const struct font *font, uint32_t cp)
{
    const unsigned char *index = font->buffer_start + map_position(font) + RANK_BYTES;
    unsigned lo = 0, hi = font->pbf.variable_count;
    while (lo < hi)
    {
        unsigned mid = lo + (hi - lo) / 2;
        const unsigned char *entry = index + mid * INDEX_BYTES;
        uint32_t code = u32(entry);
        if (cp == code)
            return entry;
        if (cp < code)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* The directory is followed by complete, native PBF cache records. */
static const unsigned char *pack_glyph(const unsigned char *pack, ucschar_t cp)
{
    unsigned lo = 0, hi = u32(pack);
    while (lo < hi)
    {
        unsigned mid = lo + (hi - lo) / 2;
        const unsigned char *entry = pack + 4 + 8 * mid;
        uint32_t code = u32(entry);
        if (code == cp)
            return pack + u32(entry + 4);
        if (code < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Four MRU-ordered ways per set. Cache payloads stay in the existing pool.
 * Each tag is verified because the backing LRU can reuse a slot. */
static const unsigned char *cached_glyph(struct font *font, ucschar_t cp)
{
    for (unsigned i = 0; i < ARRAYLEN(font->pbf.pack); i++)
    {
        if (font->pbf.pack[i])
        {
            const unsigned char *record = pack_glyph(font->pbf.pack[i], cp);
            if (record)
                return record;
        }
    }
    const unsigned char *hot = hot_glyph(font, cp);
    if (hot)
        return hot;

    unsigned set = (cp ^ (cp >> 4) ^ (cp >> 8)) & (PBF_CACHE_SETS - 1);
    uint32_t *ways = font->pbf.ways[set];
    unsigned i;
    const struct font_cache_entry *entry = NULL;
    for (i = 0; i < PBF_CACHE_WAYS && ways[i]; i++)
    {
        entry = (const void *)(font->buffer_start + ways[i]);
        if (entry->_char_code == cp && u16(entry->bitmap))
            break;
        entry = NULL;
    }
    if (!entry)
    {
        entry = font_cache_get(&font->cache, cp, true, NULL, NULL);
        if (!entry || !u16(entry->bitmap))
            return NULL;
        i = MIN(i, PBF_CACHE_WAYS - 1);
    }
    else
    {
        /* Refresh the backing LRU, too. */
        struct lru *lru = &font->cache._lru;
        const unsigned char *first = lru_data(lru, 0);
        short handle = ((const unsigned char *)entry - first) / lru->_slot_size;
        lru_touch(lru, handle);
    }
    memmove(ways + 1, ways, i * sizeof(*ways));
    ways[0] = (const unsigned char *)entry - font->buffer_start;
    return entry->bitmap;
}

static size_t phase_size(const unsigned char *p, bool rgb)
{
    unsigned pixels = (unsigned)p[2] * p[3];
    return 4 + (pixels + 1) / 2 + (rgb ? (3 * pixels + 1) / 2 : 0);
}

static bool valid_record(const unsigned char *record, size_t size)
{
    bool rgb = (record[0] & RECORD_RGB) != 0;
    unsigned phases = record[0] & ~RECORD_RGB;
    if (phases != 1 && phases != 4)
        return false;
    size_t next = 1;
    for (unsigned phase = 0; phase < phases; ++phase)
    {
        if (size - next < 4)
            return false;
        const unsigned char *p = record + next;
        next += phase_size(p, rgb);
        if (next > size)
            return false;
    }
    return next == size;
}

static bool valid_cell(const struct font_pbf *file, const unsigned char *cell)
{
    unsigned size = u16(cell);
    return size >= 5 && size <= file->max_record &&
           size <= (unsigned)file->stride - 6 - ALPHA_PADDING &&
           valid_record(cell + 6, size);
}

static void cache_read(struct font_cache_entry *entry, void *context)
{
    struct font *font = context;
    unsigned char *dest = entry->bitmap;
    memset(dest, 0, 6);
    uint32_t fixed = fixed_offset(font, entry->_char_code);
    if (fixed)
    {
        if (lseek(font->fd, fixed, SEEK_SET) == (off_t)fixed &&
            read(font->fd, dest, 6) == 6)
        {
            unsigned size = u16(dest);
            if (size >= 5 && size <= font->pbf.max_record &&
                size <= (unsigned)font->pbf.stride - 6 - ALPHA_PADDING &&
                read(font->fd, dest + 6, size) == (ssize_t)size && valid_cell(&font->pbf, dest))
            {
                memset(dest + 6 + size, 0xff, ALPHA_PADDING);
                return;
            }
        }
        memset(dest, 0, 6);
        return;
    }
    const unsigned char *index = font->pbf.stride ? variable_index(font, entry->_char_code)
                                                  : glyph_index(font, entry->_char_code);
    bool missing = !index;
    if (missing)
        index = font->pbf.stride ? variable_index(font, font->defaultchar)
                                 : glyph_index(font, font->defaultchar);
    if (!index)
        return;
    size_t size = u16(index + 8);
    uint32_t offset = u32(index + 4);
    size_t limit = font->pbf.file_size - font->pbf.bitmap_offset;
    if (size < 5 || size > font->pbf.max_record || offset > limit || size > limit - offset)
        return;
    off_t location = font->pbf.bitmap_offset + offset;
    if (lseek(font->fd, location, SEEK_SET) != location ||
        read(font->fd, dest + 6, size) != (ssize_t)size)
        return;
    if (!valid_record(dest + 6, size))
        return;
    memcpy(dest, index + 8, 6);
    /* A set high bit on the cached length marks the replacement glyph. */
    if (missing)
        dest[1] |= 0x80;
    memset(dest + 6 + size, 0xff, ALPHA_PADDING);
}

static int kern_adjust(const struct font *font, unsigned row, unsigned column)
{
    if (!row || !column || row >= font->pbf.rows)
        return 0;
    const unsigned char *rows = font->buffer_start + font->pbf.anchors * 4;
    unsigned lo = u16(rows + row * 2), hi = u16(rows + (row + 1) * 2);
    const unsigned char *pairs = rows + (font->pbf.rows + 1) * 2;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        const unsigned char *pair = pairs + mid * 3;
        if (pair[0] == column)
            return (int16_t)u16(pair + 1);
        if (pair[0] < column)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int font_pbf_run(struct font *font, struct font_run *run, ucschar_t cp,
                 struct font_glyph *out)
{
    if (cp == 0xffff)
        cp = font->defaultchar;
    const unsigned char *record = cached_glyph(font, cp);
    if (!record && font->fd >= 0 && !font_prefetch_active())
    {
        const struct font_cache_entry *entry = font_cache_get(&font->cache, cp, false, cache_read, font);
        record = entry ? entry->bitmap : NULL;
    }
    bool present = record && (u16(record) & 0x7fff);
    int advance = present ? (int)u16(record + 2) : font->maxwidth * 64;
    int origin = run->pen;
    if (present)
        origin += kern_adjust(font, run->kern_row, record[5]);
    if (IS_DIACRITIC(cp))
        origin = run->base_pen + (run->base_advance - advance) / 2;
    else
    {
        run->base_pen = origin;
        run->base_advance = advance;
        run->pen = origin + advance;
        run->kern_row = present ? record[4] : 0;
    }

    if (out)
    {
        *out = (struct font_glyph){.origin = origin, .missing = !present};
        if (present)
        {
            const unsigned char *p = record + 7;
            bool rgb = (record[6] & RECORD_RGB) != 0;
            unsigned phases = record[6] & ~RECORD_RGB;
            int rounded = origin + (phases == 4 ? 8 : 32);
            unsigned phase = phases == 4 ? (rounded >> 4) & 3 : 0;
            for (unsigned i = 0; i < phase; ++i)
                p += phase_size(p, rgb);
            out->x = (rounded >> 6) + (int8_t)p[0];
            out->y = font->ascent - (int8_t)p[1];
            out->width = p[2];
            out->height = p[3];
            out->phase = phase;
            out->bits = p + 4;
            if (rgb)
                out->rgb = out->bits + ((unsigned)p[2] * p[3] + 1) / 2;
            out->missing = (record[1] & 0x80) != 0;
        }
    }
    return (run->pen + 32) >> 6;
}

/* Only the screen thread reads or changes a font's LRU. The worker owns a
 * separate file descriptor and publishes copied records, never font pointers. */
struct pbf_record {
    uint32_t offset;
    ucschar_t codepoint;
    uint32_t data;
    unsigned char header[6];
    bool fixed;
};

struct pbf_request {
    struct font_pbf file;
    unsigned height;
    char path[MAX_PATH];
    unsigned count;
    struct pbf_record glyphs[PREFETCH_GLYPHS];
    unsigned char *hot;
};

enum batch_state { BATCH_EMPTY, BATCH_READING, BATCH_READY, BATCH_IMPORTING };

static struct pbf_request pending[MAXFONTS], job;
static struct pbf_record records[PREFETCH_GLYPHS];
static unsigned char batch_data[384 * 1024];
static unsigned char stream_buffer[32 * 1024];
static unsigned batch_count, scope_depth, scope_thread, epoch;
static int release_slot;
static bool batch_hot;
static uint32_t stream_cursor, stream_start, stream_length;
static int job_font = -1;
static enum batch_state batch;
static bool requested;
static volatile bool suspended;
static struct mutex batch_lock;
static void (*schedule_prefetch)(void);
static void (*batch_trace)(unsigned, uint32_t, unsigned);

void font_pbf_set_batch_trace(void (*trace)(unsigned, uint32_t, unsigned))
{
    batch_trace = trace;
}

static int compare_codepoint(const void *a, const void *b)
{
    ucschar_t left = ((const struct pbf_record *)a)->codepoint;
    ucschar_t right = ((const struct pbf_record *)b)->codepoint;
    return left > right ? 1 : left < right ? -1 : 0;
}

static int compare_offset(const void *a, const void *b)
{
    uint32_t left = ((const struct pbf_record *)a)->offset;
    uint32_t right = ((const struct pbf_record *)b)->offset;
    return left > right ? 1 : left < right ? -1 : 0;
}

static bool read_index_record(const struct font_pbf *file, const unsigned char *index,
                              struct pbf_record *record)
{
    uint32_t offset = u32(index + 4);
    unsigned size = u16(index + 8);
    uint32_t limit = file->file_size - file->bitmap_offset;
    if (size < 5 || size > file->max_record || offset > limit || size > limit - offset)
        return false;
    record->offset = file->bitmap_offset + offset;
    memcpy(record->header, index + 8, sizeof(record->header));
    return true;
}

/* All reads start on a block boundary and are sequential after one seek. */
static bool stream_open(int fd, uint32_t start)
{
    stream_cursor = stream_start = start & ~(BLOCK_BYTES - 1);
    stream_length = 0;
    return lseek(fd, stream_cursor, SEEK_SET) == (off_t)stream_cursor;
}

static bool stream_at(int fd, uint32_t offset, unsigned char *out, size_t size, uint32_t end)
{
    while (size && !suspended)
    {
        if (offset >= stream_start + stream_length)
        {
            stream_start = stream_cursor;
            if (stream_cursor >= end)
                return false;
            stream_length = MIN(sizeof(stream_buffer), end - stream_cursor);
            if (!stream_length || read(fd, stream_buffer, stream_length) != (ssize_t)stream_length)
                return false;
            stream_cursor += stream_length;
            yield();
            continue;
        }
        if (offset < stream_start)
            return false;
        size_t n = MIN(size, stream_start + stream_length - offset);
        memcpy(out, stream_buffer + offset - stream_start, n);
        out += n;
        offset += n;
        size -= n;
    }
    return !size;
}

static uint32_t block_end(uint32_t end)
{
    return MIN(job.file.file_size, (end + BLOCK_BYTES - 1) & ~(BLOCK_BYTES - 1));
}

/* Pre-FIX1 fonts remain readable via their original sorted directory. */
static bool read_legacy_index(int fd)
{
    qsort(records, job.count, sizeof(*records), compare_codepoint);
    struct pbf_record replacement = {0};
    unsigned wanted = 0;
    uint32_t offset = job.file.index_offset, end = block_end(job.file.bitmap_offset);
    while (offset < job.file.bitmap_offset)
    {
        unsigned char index[INDEX_BYTES];
        if (!stream_at(fd, offset, index, sizeof(index), end))
            return false;
        offset += INDEX_BYTES;
        uint32_t cp = u32(index);
        while (wanted < job.count && records[wanted].codepoint < cp)
            wanted++;
        if (cp == 0xfffd && !read_index_record(&job.file, index, &replacement))
            return false;
        if (wanted < job.count && records[wanted].codepoint == cp &&
            !read_index_record(&job.file, index, &records[wanted++]))
            return false;
    }
    if (!replacement.offset)
        return false;
    for (unsigned i = 0; i < job.count; i++)
    {
        if (records[i].offset)
            continue;
        replacement.codepoint = records[i].codepoint;
        records[i] = replacement;
        records[i].header[1] |= 0x80;
    }
    return true;
}

static unsigned record_disk_bytes(const struct pbf_record *record)
{
    return record->fixed ? job.file.stride : u16(record->header) & 0x7fff;
}

static bool load_hot(int fd, uint32_t end)
{
    if (!stream_at(fd, job.file.hot_offset, job.hot, hot_bytes(&job.file), end))
        return false;
    for (unsigned i = 0; i < job.file.hot_count; i++)
        if (!valid_cell(&job.file, job.hot + i * job.file.stride))
            return false;
    return true;
}

static unsigned read_batch(int fd, bool *loaded_hot)
{
    memcpy(records, job.glyphs, job.count * sizeof(*records));
    if (!job.file.stride &&
        (!stream_open(fd, job.file.index_offset) || !read_legacy_index(fd)))
        return 0;

    qsort(records, job.count, sizeof(*records), compare_offset);
    uint32_t start = records[0].offset, end = 0;
    for (unsigned i = 0; i < job.count; i++)
        end = MAX(end, records[i].offset + record_disk_bytes(&records[i]));
    if (job.hot)
    {
        start = MIN(start, job.file.hot_offset);
        end = MAX(end, job.file.hot_offset + hot_bytes(&job.file));
    }
    end = block_end(end);
    if (job.file.stride && !stream_open(fd, start))
        return 0;

    unsigned used = 0, copied = 0;
    for (; copied < job.count && !suspended; copied++)
    {
        struct pbf_record *r = &records[copied];
        if (job.hot && !*loaded_hot && r->offset >= job.file.hot_offset)
        {
            *loaded_hot = load_hot(fd, end);
            if (!*loaded_hot)
                return copied;
        }
        if (copied && records[copied - 1].offset == r->offset)
        {
            r->data = records[copied - 1].data;
            continue;
        }

        unsigned prefix = r->fixed ? 6 : 0;
        unsigned disk_bytes = record_disk_bytes(r);
        if (disk_bytes > sizeof(batch_data) - used)
            return copied;
        unsigned char *out = batch_data + used;
        if (*loaded_hot && r->fixed && r->offset >= job.file.hot_offset &&
            r->offset < job.file.hot_offset + hot_bytes(&job.file))
            memcpy(out, job.hot + r->offset - job.file.hot_offset, disk_bytes);
        else if (!stream_at(fd, r->offset, out, disk_bytes, end))
            return copied;

        if (r->fixed)
        {
            if (!valid_cell(&job.file, out))
                return copied;
            memcpy(r->header, out, sizeof(r->header));
        }
        else if (!valid_record(out, disk_bytes))
            return copied;
        r->data = used + prefix;
        used += disk_bytes;
    }
    /* A batch can contain only variable glyphs that precede the hot block. */
    if (job.hot && !*loaded_hot && !suspended)
        *loaded_hot = load_hot(fd, end);
    return copied;
}

void font_pbf_prefetch_init(void (*schedule)(void))
{
    mutex_init(&batch_lock);
    schedule_prefetch = schedule;
}

void font_pbf_prefetch_work(void)
{
    mutex_lock(&batch_lock);
    if (suspended || batch != BATCH_EMPTY)
    {
        mutex_unlock(&batch_lock);
        return;
    }
    int id = -1;
    for (int offset = 1; offset <= MAXFONTS; offset++)
    {
        int next = (job_font + offset) % MAXFONTS;
        if (pending[next].count)
        {
            id = next;
            break;
        }
    }
    if (id < 0)
    {
        mutex_unlock(&batch_lock);
        return;
    }
    job_font = id;
    job = pending[id];
    pending[id].count = 0;
    batch = BATCH_READING;
    mutex_unlock(&batch_lock);

    uint32_t start = batch_trace ? USEC_TIMER : 0;
    unsigned count = 0;
    bool loaded_hot = false;
    int fd = open(job.path, O_RDONLY);
    if (fd >= 0)
    {
        if (ffilesize(fd) == (off_t)job.file.file_size)
            count = read_batch(fd, &loaded_hot);
        close(fd);
    }

    if (batch_trace)
        batch_trace(job.height, USEC_TIMER - start, count);
    mutex_lock(&batch_lock);
    batch_count = suspended ? 0 : count;
    batch_hot = !suspended && loaded_hot;
    batch = BATCH_READY;
    mutex_unlock(&batch_lock);
}

static void publish_record(struct font_cache_entry *entry, void *context)
{
    const struct pbf_record *r = context;
    unsigned bytes = u16(r->header) & 0x7fff;
    memcpy(entry->bitmap, r->header, 6);
    memcpy(entry->bitmap + 6, batch_data + r->data, bytes);
    memset(entry->bitmap + 6 + bytes, 0xff, ALPHA_PADDING);
}

unsigned font_prefetch_epoch(void)
{
    if (!schedule_prefetch)
        return epoch;
    mutex_lock(&batch_lock);
    if (batch != BATCH_READY)
    {
        mutex_unlock(&batch_lock);
        return epoch;
    }
    batch = BATCH_IMPORTING;
    mutex_unlock(&batch_lock);

    int id = job_font;
    if ((batch_count || batch_hot) && !suspended)
    {
        font_lock(id, true);
        struct font *font = font_get(id);
        if (font->pbf.serial == job.file.serial && !font->disabled)
        {
            font->pbf.hot_ready |= batch_hot;
            for (unsigned i = 0; i < batch_count; i++)
            {
                if (hot_glyph(font, records[i].codepoint))
                    continue;
                struct font_cache_entry *entry = font_cache_get(
                    &font->cache, records[i].codepoint, false, publish_record, &records[i]);
                /* A failing read may have left an empty entry at this key. */
                if (!u16(entry->bitmap))
                    publish_record(entry, &records[i]);
            }
            ++epoch;
        }
        font_lock(id, false);
    }

    if (release_slot)
    {
        hot_used[release_slot - 1] = false;
        release_slot = 0;
    }
    mutex_lock(&batch_lock);
    batch = BATCH_EMPTY;
    mutex_unlock(&batch_lock);
    schedule_prefetch();
    return epoch;
}

bool font_prefetch_busy(void)
{
    if (!schedule_prefetch || suspended)
        return false;
    mutex_lock(&batch_lock);
    bool busy = batch != BATCH_EMPTY;
    for (int i = 0; !busy && i < MAXFONTS; i++)
        busy = pending[i].count != 0;
    mutex_unlock(&batch_lock);
    return busy;
}

void font_prefetch_begin(void)
{
    if (!scope_depth)
    {
        font_prefetch_epoch();
        scope_thread = thread_self();
    }
    ++scope_depth;
}

bool font_prefetch_active(void)
{
    return schedule_prefetch && scope_depth && scope_thread == thread_self();
}

void font_prefetch_end(void)
{
    if (--scope_depth == 0 && requested && schedule_prefetch)
    {
        requested = false;
        schedule_prefetch();
    }
}

void font_pbf_prefetch_suspend(bool suspend)
{
    suspended = suspend;
    if (!schedule_prefetch)
        return;
    mutex_lock(&batch_lock);
    if (suspend)
    {
        for (int i = 0; i < MAXFONTS; i++)
            pending[i].count = 0;
        if (batch == BATCH_READY)
        {
            batch_count = 0;
            batch_hot = false;
        }
    }
    mutex_unlock(&batch_lock);
}

void font_pbf_forget(struct font *font, int id)
{
    int slot = font->pbf.hot_slot;
    if (schedule_prefetch)
    {
        mutex_lock(&batch_lock);
        if (pending[id].file.serial == font->pbf.serial)
            pending[id].count = 0;
        if (slot && batch != BATCH_EMPTY && job_font == id &&
            job.file.serial == font->pbf.serial && job.hot)
        {
            release_slot = slot;
            slot = 0;
        }
        mutex_unlock(&batch_lock);
    }
    if (slot)
        hot_used[slot - 1] = false;
}

static bool contains(const struct pbf_request *request, ucschar_t cp)
{
    for (unsigned i = 0; i < request->count; i++)
        if (request->glyphs[i].codepoint == cp)
            return true;
    return false;
}

static bool resolve(const struct font *font, ucschar_t cp, struct pbf_record *r)
{
    *r = (struct pbf_record){.codepoint = cp};
    if (!font->pbf.stride)
        return true;

    r->offset = fixed_offset(font, cp);
    if (r->offset)
    {
        r->fixed = true;
        return true;
    }
    const unsigned char *index = variable_index(font, cp);
    bool missing = !index;
    if (missing)
        index = variable_index(font, font->defaultchar);
    if (!index || !read_index_record(&font->pbf, index, r))
        return false;
    if (missing)
        r->header[1] |= 0x80;
    return true;
}

/* The shared worker serializes this scratch with ordinary PBF batch reads. */
static struct pbf_record pack_records[FONT_PACK_CODES];

static void put_u32(unsigned char *out, uint32_t val)
{
    for (unsigned i = 0; i < 4; i++)
        out[i] = val >> (8 * i);
}

uint32_t font_pbf_pack_revision(int id)
{
    if (id < 0)
        return 0;
    font_lock(id, true);
    struct font *font = font_get(id);
    uint32_t result = crc_32(font->buffer_start, resident_bytes(font), font->pbf.file_size);
    result = crc_32(font->buffer_start + map_position(font), map_bytes(&font->pbf), result);
    font_lock(id, false);
    return result;
}

size_t font_pbf_pack_build(int id, const char *path, const ucschar_t *codes,
                           unsigned count, unsigned char *out, size_t size,
                           bool (*cancelled)(void))
{
    size_t used = 4 + 8 * count;
    if (id < 0 || count > FONT_PACK_CODES || used > size)
        return 0;
    font_lock(id, true);
    const struct font *font = font_get(id);
    int fd = open(path, O_RDONLY);
    if (fd < 0 || !font->pbf.stride)
        goto fail;
    uint32_t end = font->pbf.file_size;
    if (ffilesize(fd) != (off_t)end)
        goto fail;

    put_u32(out, count);
    for (unsigned i = 0; i < count; i++)
    {
        ucschar_t cp = codes[i] == 0xffff ? (ucschar_t)font->defaultchar : codes[i];
        if (!resolve(font, cp, &pack_records[i]))
            goto fail;
        pack_records[i].data = i;
        put_u32(out + 4 + 8 * i, codes[i]);
    }
    qsort(pack_records, count, sizeof(*pack_records), compare_offset);
    uint32_t last = count ? pack_records[count - 1].offset : 0;
    uint32_t limit = MIN(end, (last + font->pbf.max_record + 6 +
                              BLOCK_BYTES - 1) & ~(BLOCK_BYTES - 1));
    stream_length = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const struct pbf_record *r = &pack_records[i];
        if (suspended || cancelled())
            goto fail;
        /* On a rotating disk, a short sequential skip is cheaper than a
         * seek. Reposition only when the next record is beyond 128 KiB. */
        if ((!i || (r->offset >= stream_cursor &&
                    r->offset - stream_cursor >= 128 * 1024)) &&
            !stream_open(fd, r->offset))
            goto fail;

        unsigned char prefix[6];
        if (r->fixed)
        {
            if (!stream_at(fd, r->offset, prefix, sizeof(prefix), limit))
                goto fail;
        }
        else
            memcpy(prefix, r->header, sizeof(prefix));
        unsigned bytes = u16(prefix) & 0x7fff;
        if (bytes < 5 || bytes > font->pbf.max_record ||
            size - used < sizeof(prefix) + bytes + ALPHA_PADDING)
            goto fail;
        put_u32(out + 8 + 8 * r->data, used);
        memcpy(out + used, prefix, sizeof(prefix));
        if (!stream_at(fd, r->offset + (r->fixed ? sizeof(prefix) : 0),
                       out + used + sizeof(prefix), bytes, limit) ||
            !valid_record(out + used + sizeof(prefix), bytes))
            goto fail;
        memset(out + used + sizeof(prefix) + bytes, 0xff, ALPHA_PADDING);
        used += sizeof(prefix) + bytes + ALPHA_PADDING;
    }
    close(fd);
    font_lock(id, false);
    return used;

fail:
    if (fd >= 0)
        close(fd);
    font_lock(id, false);
    return 0;
}

bool font_pbf_pack_valid(int id, const unsigned char *pack, size_t size)
{
    if (id < 0 || size < 4)
        return false;
    unsigned count = u32(pack);
    if (count > FONT_PACK_CODES || 4 + 8 * count > size)
        return false;
    font_lock(id, true);
    const struct font *font = font_get(id);
    bool valid = true;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const unsigned char *entry = pack + 4 + 8 * i;
        uint32_t cp = u32(entry), offset = u32(entry + 4);
        if ((i && cp <= previous) || cp > 0x10ffff ||
            offset < 4 + 8 * count || offset > size - 4 ||
            size - offset < 6 + ALPHA_PADDING)
        {
            valid = false;
            break;
        }
        unsigned length = u16(pack + offset) & 0x7fff;
        if (length < 5 || length > font->pbf.max_record ||
            size - offset < 6 + length + ALPHA_PADDING ||
            !valid_record(pack + offset + 6, length))
        {
            valid = false;
            break;
        }
        previous = cp;
    }
    font_lock(id, false);
    return valid;
}

void font_pbf_pack_bind(int id, const unsigned char *primary, const unsigned char *adjacent)
{
    if (id < 0)
        return;
    struct font *font = font_get(id);
    font->pbf.pack[0] = primary;
    font->pbf.pack[1] = adjacent;
}


bool font_pbf_prefetch(struct font *font, int id, const char *path,
                       const ucschar_t *text)
{
    /* With no background thread, the ordinary font renderer can still
     * use its synchronous cache-fill path. */
    if (!text || !schedule_prefetch)
        return true;
    bool ready = true;
    for (const ucschar_t *p = text; *p; p++)
    {
        ucschar_t cp = *p == 0xffff ? (ucschar_t)font->defaultchar : *p;
        ready &= cached_glyph(font, cp) != NULL;
    }
    if (ready || suspended || font->disabled)
        return ready;

    if (hot_memory && font->pbf.want_hot && !font->pbf.hot_slot && font->pbf.stride)
    {
        for (unsigned i = 0; i < ARRAYLEN(hot_slots); i++)
        {
            if (hot_slots[i].height != font->height || hot_slots[i].capacity < hot_bytes(&font->pbf) ||
                hot_used[i])
                continue;
            hot_used[i] = true;
            font->pbf.hot_slot = i + 1;
            break;
        }
    }

    mutex_lock(&batch_lock);
    struct pbf_request *request = &pending[id];
    if (request->file.serial != font->pbf.serial)
    {
        request->file = font->pbf;
        request->height = font->height;
        strlcpy(request->path, path, sizeof(request->path));
        request->count = 0;
    }
    unsigned limit = MIN(PREFETCH_GLYPHS, font->cache._capacity);
    bool in_flight = batch != BATCH_EMPTY && job_font == id &&
                     job.file.serial == font->pbf.serial;
    for (; *text && request->count < limit; text++)
    {
        ucschar_t cp = *text == 0xffff ? (ucschar_t)font->defaultchar : *text;
        if (cached_glyph(font, cp) || contains(request, cp) || (in_flight && contains(&job, cp)))
            continue;
        if (!resolve(font, cp, &request->glyphs[request->count]))
            continue;
        request->count++;
        requested = true;
    }
    if (request->count)
        request->hot = font->pbf.hot_slot && !font->pbf.hot_ready &&
                       !(in_flight && job.hot) ? hot_data(font->pbf.hot_slot) : NULL;
    mutex_unlock(&batch_lock);
    if (!scope_depth && requested)
    {
        requested = false;
        schedule_prefetch();
    }
    return false;
}
