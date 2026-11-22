/* Callable image decoder overlay ABI. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_IMAGE_OVERLAY_H
#define ASTRAPOD_ROCK_IMAGE_OVERLAY_H

#define ROCK_IMAGE_DECODER_MAGIC 0x41535449UL
#define ROCK_IMAGE_DECODER_OFFSET 0x200000

#ifndef __ASSEMBLER__
#include <stddef.h>

#define ROCK_IMAGE_DECODER_ABI 1u

struct image_decoder;
struct imgdec_api;

struct rock_image_decoder_request {
    unsigned int version;
    size_t parent_api_size;
    size_t decoder_size;
    const struct imgdec_api *parent_api;
    const struct image_decoder *decoder;
};
#endif

#endif
