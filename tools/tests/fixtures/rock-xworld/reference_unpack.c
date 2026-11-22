/* Upstream XWorld bank decompressor with every access checked. The
 * algorithm follows apps/plugins/xworld/bank.c; the probe compares the
 * staged decoder against it. GPL-2.0-or-later. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reference_unpack.h"

struct ref {
    const uint8_t *start;
    uint8_t *out_start;
    long in, out;   /* byte indexes into the shared buffer */
    size_t limit;
    uint32_t crc, chk;
    int32_t datasize;
    uint16_t size;
    bool fault;
};

static uint32_t word_at(struct ref *r, long at)
{
    if (at < 0 || (size_t)at + 4 > r->limit) {
        r->fault = true;
        return 0;
    }
    const uint8_t *p = r->start + at;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static bool rcr(struct ref *r, bool cf)
{
    bool out = r->chk & 1;
    r->chk >>= 1;
    if (cf)
        r->chk |= 0x80000000;
    return out;
}

static bool next_chunk(struct ref *r)
{
    bool cf = rcr(r, false);
    if (r->chk == 0) {
        if (r->in < 0) {
            r->fault = true;
            return false;
        }
        r->chk = word_at(r, r->in);
        r->in -= 4;
        r->crc ^= r->chk;
        cf = rcr(r, true);
    }
    return cf;
}

static uint16_t get_code(struct ref *r, uint8_t bits)
{
    uint16_t c = 0;
    while (bits-- && !r->fault)
        c = c << 1 | next_chunk(r);
    return c;
}

static void put(struct ref *r, uint8_t byte)
{
    /* Output must not reach the next unread input word. */
    if (r->out < r->in + 4 || r->out < 0 || (size_t)r->out >= r->limit) {
        r->fault = true;
        return;
    }
    r->out_start[r->out--] = byte;
}

static void dec_unk1(struct ref *r, uint8_t bits, uint8_t add)
{
    uint16_t count = get_code(r, bits) + add + 1;
    r->datasize -= count;
    while (count-- && !r->fault)
        put(r, (uint8_t)get_code(r, 8));
}

static void dec_unk2(struct ref *r, uint8_t bits, long written_end)
{
    uint16_t i = get_code(r, bits);
    uint16_t count = r->size + 1;
    r->datasize -= count;
    while (count-- && !r->fault) {
        long from = r->out + i;
        /* Upstream reads any byte; only produced output is meaningful. */
        if (i == 0 || from > written_end) {
            r->fault = true;
            return;
        }
        put(r, r->out_start[from]);
    }
}

int reference_unpack(uint8_t *buf, size_t packed, size_t unpacked,
                     uint32_t *residue)
{
    struct ref r = { .start = buf, .out_start = buf, .limit = unpacked };
    if (packed < 12 || packed >= unpacked)
        return REF_FAULT;
    r.in = (long)packed - 4;
    r.datasize = word_at(&r, r.in);
    r.in -= 4;
    if (r.datasize <= 0 || (size_t)r.datasize != unpacked)
        return REF_FAULT;
    r.out = r.datasize - 1;
    long written_end = r.out;
    r.crc = word_at(&r, r.in);
    r.in -= 4;
    r.chk = word_at(&r, r.in);
    r.in -= 4;
    r.crc ^= r.chk;
    /* Packed data always carries the sentinel bit in its first word. */
    if (!r.chk)
        return REF_FAULT;
    do {
        if (!next_chunk(&r)) {
            r.size = 1;
            if (!next_chunk(&r))
                dec_unk1(&r, 3, 0);
            else
                dec_unk2(&r, 8, written_end);
        } else {
            uint16_t c = get_code(&r, 2);
            if (c == 3) {
                dec_unk1(&r, 8, 8);
            } else if (c < 2) {
                r.size = c + 2;
                dec_unk2(&r, c + 9, written_end);
            } else {
                r.size = get_code(&r, 8);
                dec_unk2(&r, 12, written_end);
            }
        }
    } while (!r.fault && r.datasize > 0);
    if (r.fault || r.datasize != 0)
        return REF_FAULT;
    *residue = r.crc;
    return r.crc == 0 ? REF_OK : REF_BAD_CRC;
}
