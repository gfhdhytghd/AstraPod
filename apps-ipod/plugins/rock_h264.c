/* Foreground hardware decoder lease. GNU GPL version 2 or later. */
#include "kernel.h"
#include "plugins/rock_h264.h"

static unsigned int owner;
static uintptr_t arena_start;
static size_t arena_size;
static struct vpu_h264 *active;

void rock_h264_begin(void *scratch, size_t capacity)
{
    owner = thread_self();
    arena_start = (uintptr_t)scratch;
    arena_size = capacity;
}

void rock_h264_end(void)
{
    if (active)
    {
        target_hw_h264_api.close(active);
        active = NULL;
    }
    owner = 0;
    arena_start = 0;
    arena_size = 0;
}

static bool owns(struct vpu_h264 const *decoder)
{
    return owner && owner == thread_self() && active && decoder == active;
}

static size_t decoder_size(int w, int h)
{
    if (w <= 0 || w > 640 || h <= 0 || h > 480 || w % 16 || h % 16)
        return 0;
    return target_hw_h264_api.buf_size(w, h);
}

static struct vpu_h264 *decoder_open(void *scratch, size_t size, int w, int h)
{
    uintptr_t address = (uintptr_t)scratch;
    size_t needed = decoder_size(w, h);
    if (!owner || owner != thread_self() || active || !needed ||
        !scratch || address % 32 || address < arena_start ||
        size > arena_size || address - arena_start > arena_size - size ||
        size < needed)
        return NULL;

    active = target_hw_h264_api.open(scratch, size, w, h);
    return active;
}

static int decoder_configure(struct vpu_h264 *decoder,
                             const uint8_t *avcc, int length)
{
    return owns(decoder) ?
        target_hw_h264_api.configure(decoder, avcc, length) : -1;
}

static int decoder_decode(struct vpu_h264 *decoder, const uint8_t *sample,
                          int length, int nalu_len_size)
{
    return owns(decoder) ?
        target_hw_h264_api.decode_sample(decoder, sample, length,
                                         nalu_len_size) : -1;
}

static void decoder_frame(const struct vpu_h264 *decoder,
                          const uint8_t **y, const uint8_t **cb,
                          const uint8_t **cr, int *w, int *h, int *stride)
{
    if (owns(decoder))
        target_hw_h264_api.get_frame(decoder, y, cb, cr, w, h, stride);
}

static void decoder_close(struct vpu_h264 *decoder)
{
    if (owns(decoder))
    {
        target_hw_h264_api.close(decoder);
        active = NULL;
    }
}

const struct hw_h264_api rock_h264_api = {
    .buf_size = decoder_size,
    .open = decoder_open,
    .configure = decoder_configure,
    .decode_sample = decoder_decode,
    .get_frame = decoder_frame,
    .close = decoder_close,
};
