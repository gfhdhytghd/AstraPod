#ifndef FLAC_OUTPUT_H
#define FLAC_OUTPUT_H
#include <stdint.h>
#include <stdbool.h>
#define CONVERT_BLOCK 1024
struct flac_output {
    int fd;
    unsigned frame_number, used;
    uint64_t written, expected;
    int16_t pcm[2 * CONVERT_BLOCK];
    uint8_t frame[4 * CONVERT_BLOCK + 16];
    struct convert_md5 md5;
};
#endif
