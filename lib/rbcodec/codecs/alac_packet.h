#ifndef ALAC_PACKET_H
#define ALAC_PACKET_H
/* ALAC channel element, partial-block flag and 32-bit sample count are
 * bit fields. A partial count starts in the last bit of the third byte. */
static unsigned alac_packet_samples(const unsigned char *p, size_t size,
                                    unsigned channels, unsigned depth)
{
    if (!p || size < 3)
        return 0;
    unsigned element = p[0] >> 5, shifted = (p[2] >> 2) & 3;
    bool escape = (p[2] & 2) != 0;
    if ((channels == 2 && element != 1) ||
        (channels == 1 && element != 0 && element != 3))
        return 0;
    if ((depth == 16 && !escape && shifted != 0) ||
        (depth == 24 && (element == 3 || shifted != (escape ? 0u : 1u))))
        return 0;
    if (!(p[2] & 0x10))
        return 4096;
    if (size < 7 || (p[2] & 1) || p[3] || (p[4] & 0xe0))
        return 0;
    unsigned n = ((unsigned)p[4] << 15) | ((unsigned)p[5] << 7) | (p[6] >> 1);
    return n <= 4096 ? n : 0;
}
#endif
