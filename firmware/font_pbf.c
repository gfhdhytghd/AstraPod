/* Pixel-native PodBox bitmap font reader. GPL v2+. */
#include "font_pbf.h"
#include "file.h"
#include "system.h"
#include "diacritic.h"
#include <string.h>

enum {
    HEADER_BYTES = 36,
    PAGE_ENTRIES = 64,
    INDEX_BYTES = 14,
    CACHE_BUDGET = 108 * 1024,
    MAX_RECORD = 16384,
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

static size_t cache_offset(const struct font *font)
{
    return (resident_bytes(font) + PAGE_ENTRIES * INDEX_BYTES + 3) & ~3u;
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
    size_t cap = CACHE_BUDGET / entry;
    if (limit > cache_offset(font))
        cap = MIN(cap, (limit - cache_offset(font)) / entry);
    /* The existing LRU aligns its index for 32-bit Unicode. */
    size_t slots = MAX(2, MIN(cap, (unsigned)MAX(2, glyphs))) & ~1u;
    return cache_offset(font) + slots * entry;
}

bool font_pbf_open(struct font *font)
{
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

    size_t offset = cache_offset(font);
    font_cache_create(&font->cache, buf + offset, font->buffer_size - offset,
                      6 + font->pbf.max_record + ALPHA_PADDING);
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

static void cache_read(struct font_cache_entry *entry, void *context)
{
    struct font *font = context;
    unsigned char *dest = entry->bitmap;
    memset(dest, 0, 6);
    const unsigned char *index = glyph_index(font, entry->_char_code);
    bool missing = !index;
    if (missing)
        index = glyph_index(font, font->defaultchar);
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
    unsigned phases = dest[6];
    if (phases != 1 && phases != 4)
        return;
    size_t next = 1;
    for (unsigned phase = 0; phase < phases; ++phase)
    {
        if (size - next < 4)
            return;
        const unsigned char *p = dest + 6 + next;
        next += 4 + ((unsigned)p[2] * p[3] + 1) / 2;
        if (next > size)
            return;
    }
    if (next != size)
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
    const struct font_cache_entry *entry = font_cache_get(
        &font->cache, cp, font->fd < 0, cache_read, font);
    const unsigned char *record = entry ? entry->bitmap : NULL;
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
            unsigned phases = record[6];
            int rounded = origin + (phases == 4 ? 8 : 32);
            unsigned phase = phases == 4 ? (rounded >> 4) & 3 : 0;
            for (unsigned i = 0; i < phase; ++i)
                p += 4 + ((unsigned)p[2] * p[3] + 1) / 2;
            out->x = (rounded >> 6) + (int8_t)p[0];
            out->y = font->ascent - (int8_t)p[1];
            out->width = p[2];
            out->height = p[3];
            out->phase = phase;
            out->bits = p + 4;
            out->missing = (record[1] & 0x80) != 0;
        }
    }
    return (run->pen + 32) >> 6;
}
