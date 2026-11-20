/* A bounded FLAC encoder for stereo signed 16-bit 48 kHz samples.
 * Verbatim subframes preserve every output sample and bound encoding time. */
#include "convert_md5.inc"
#include "flac_output.h"

static unsigned flac_utf8_frame(uint8_t *dst, unsigned value)
{
    if (value < 128) { dst[0] = value; return 1; }
    unsigned bytes = 2;
    while (bytes < 6 && value >= (1u << (5 * bytes + 1))) ++bytes;
    for (unsigned k = bytes - 1; k; --k) {
        dst[k] = 0x80 | (value & 0x3f);
        value >>= 6;
    }
    dst[0] = (0xffu << (8 - bytes)) | value;
    return bytes;
}

static bool flac_put(int fd, const void *src, size_t bytes)
{
    const uint8_t *p = src;
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n <= 0) return false;
        p += n;
        bytes -= n;
    }
    return true;
}

static bool flac_out_header(struct flac_output *o, const uint8_t *md5)
{
    uint8_t h[42] = { 'f','L','a','C', 0,0,0,34, 4,0,4,0 };
    /* STREAMINFO has 20 rate bits, 3 channel bits, 5 depth bits and 36 count bits. */
    uint64_t format = ((uint64_t)48000 << 44) | ((uint64_t)1 << 41) |
                      ((uint64_t)15 << 36) | o->expected;
    for (int i=7; i>=0; --i) { h[18+i]=format; format >>= 8; }
    if (md5) memcpy(h+26,md5,16);
    return lseek(o->fd,0,SEEK_SET)==0 && flac_put(o->fd,h,sizeof h);
}

static bool flac_out_open(struct flac_output *o, int fd, uint64_t expected)
{
    if (!expected || expected >= (1ULL << 36)) return false;
    o->fd=fd; o->expected=expected; o->frame_number=0; o->written=0; o->used=0;
    conv_md5_starts(&o->md5);
    return flac_out_header(o,NULL);
}

static bool flac_out_frame(struct flac_output *o)
{
    unsigned pairs=o->used;
    uint8_t *p=o->frame;
    *p++=0xff; *p++=0xf8; *p++=0x7a; *p++=0x18;
    p+=flac_utf8_frame(p,o->frame_number++);
    *p++=(pairs-1)>>8; *p++=pairs-1;
    uint8_t crc8=0;
    for (uint8_t *b=o->frame;b<p;++b) {
        crc8^=*b;
        for (int bit=0;bit<8;++bit) crc8=(crc8<<1)^((crc8&128)?7:0);
    }
    *p++=crc8;
    for (int ch=0;ch<2;++ch) {
        *p++=2;
        for (unsigned i=0;i<pairs;++i) {
            uint16_t v=o->pcm[2*i+ch];
            *p++=v>>8; *p++=v;
        }
    }
    uint16_t crc=0;
    for (uint8_t *b=o->frame;b<p;++b) {
        crc^=(uint16_t)*b<<8;
        for (int bit=0;bit<8;++bit) crc=(crc<<1)^((crc&0x8000)?0x8005:0);
    }
    *p++=crc>>8; *p++=crc;
    if (!flac_put(o->fd,o->frame,p-o->frame)) return false;
    /* S5L8702 and the host reference both use signed little-endian samples. */
    conv_md5_update(&o->md5,(uint8_t*)o->pcm,4*pairs);
    o->written+=pairs; o->used=0;
    return true;
}

static bool flac_out_add(struct flac_output *o,const int16_t *pcm,unsigned pairs)
{
    if (pairs>o->expected-o->written-o->used) return false;
    while (pairs) {
        unsigned room=CONVERT_BLOCK-o->used;
        unsigned n=pairs<room?pairs:room;
        memcpy(o->pcm+2*o->used,pcm,4*n);
        o->used+=n; pcm+=2*n; pairs-=n;
        if (o->used==CONVERT_BLOCK && !flac_out_frame(o)) return false;
    }
    return true;
}

static bool flac_out_finish(struct flac_output *o)
{
    uint8_t digest[16];
    if (o->written+o->used!=o->expected || (o->used && !flac_out_frame(o))) return false;
    conv_md5_finish(&o->md5,digest);
    return flac_out_header(o,digest) && fsync(o->fd)==0;
}
