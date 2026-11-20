#include "sm1_flac_profile.h"
/* ARM-side compressed frame boundary reader. No sample reconstruction. */
#ifndef SM1_FLAC_FRAME_H
#define SM1_FLAC_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct sm1_header { unsigned block, number; bool variable; };

static uint16_t sm1_crc16_step(uint16_t crc, unsigned byte)
{
    crc ^= byte << 8;
    for (unsigned i = 0; i < 8; ++i)
        crc = (crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0);
    return crc;
}

static bool sm1_flac_header(const uint8_t *p, size_t n, unsigned rate, unsigned depth, struct sm1_header *out)
{
    if (n < 6 || p[0] != 0xff || (p[1] & 0xfe) != 0xf8 ||
        (p[3] >> 4 != 1 && (p[3] >> 4 < 8 || p[3] >> 4 > 10)) ||
        ((p[3] & 15) != 0 && (p[3] & 15) != (depth == 24 ? 12 : 8)))
        return false;
    unsigned bc = p[2] >> 4, rc = p[2] & 15, k = 4, v = p[k++];
    unsigned number;
    if (v < 128)
        number = v;
    else {
        unsigned bits = 0;
        while (bits < 8 && (v & (0x80 >> bits))) ++bits;
        if (bits < 2 || bits > 6 || k + bits > n)
            return false;
        number = v & ((1u << (7 - bits)) - 1);
        for (unsigned i = 1; i < bits; ++i) {
            if ((p[k] & 0xc0) != 0x80) return false;
            number = (number << 6) | (p[k++] & 63);
        }
        unsigned minimum = bits == 2 ? 128 : 1u << (5 * bits - 4);
        if (number < minimum) return false;
    }
    unsigned block;
    if (bc == 1) block = 192;
    else if (bc >= 2 && bc <= 5) block = 576u << (bc - 2);
    else if (bc == 6 && k < n) block = 1u + p[k++];
    else if (bc == 7 && k + 2 <= n) { block = 1u + (p[k] << 8) + p[k+1]; k += 2; }
    else if (bc >= 8) block = 256u << (bc - 8);
    else return false;
    const struct sm1_flac_profile *profile=sm1_profile_for(rate,depth);
    if (!profile || block > profile->max_block) return false;
    unsigned hz;
    if (!rc) hz = rate;
    else if (rc == 9) hz = 44100;
    else if (rc == 10) hz = 48000;
    else if (rc == 11) hz = 96000;
    else if (rc == 3) hz = 192000;
    else if (rc == 12 && k < n) hz = p[k++] * 1000;
    else if ((rc == 13 || rc == 14) && k + 2 <= n) {
        hz = (p[k] << 8) + p[k+1]; k += 2;
        if (rc == 14) hz *= 10;
    } else return false;
    if (hz != rate || k >= n) return false;
    uint8_t crc = 0;
    for (unsigned i = 0; i <= k; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80) ? 7 : 0);
    }
    if (crc) return false;
    out->block = block; out->number = number; out->variable = p[1] & 1;
    return true;
}

static unsigned sm1_frame_length(const uint8_t *p, unsigned n, unsigned bound,
                                 unsigned rate, unsigned depth, unsigned frame, unsigned sample,
                                 unsigned total, bool at_end, struct sm1_header *h)
{
    if (!sm1_flac_header(p, n, rate, depth, h) || h->number != (h->variable ? sample : frame) ||
        sample >= total || h->block > total - sample)
        return 0;
    unsigned nextsample = sample + h->block;
    uint16_t crc = 0;
    for (unsigned j = 1; j <= n && j <= bound; ++j) {
        crc = sm1_crc16_step(crc, p[j-1]);
        if (crc || j < 8) continue;
        if (j == n)
            return at_end && nextsample == total ? j : 0;
        if (nextsample < total && p[j] == 0xff) {
            struct sm1_header next;
            if (sm1_flac_header(p+j, n-j, rate, depth, &next) &&
                next.variable == h->variable && next.number == (h->variable ? nextsample : frame + 1))
                return j;
        }
    }
    return 0;
}
#endif
