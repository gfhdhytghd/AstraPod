/* Foreground hardware decoder lease. GNU GPL version 2 or later. */
#include "kernel.h"
#include "lcd.h"
#include "plugins/rock_execution.h"
#include "plugins/rock_h264.h"

static unsigned int owner, decode_worker;
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
    owner = decode_worker = 0;
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

static bool owns_decode(const struct vpu_h264 *decoder)
{
    return owns(decoder) ||
        (owner && active && decoder == active &&
         decode_worker == thread_self());
}

static bool set_decode_worker(const struct vpu_h264 *decoder, unsigned int id)
{
    if (!owns(decoder) || (id && !rock_worker_owned(id)))
        return false;
    decode_worker = id;
    return true;
}

static uint32_t rock_time_us(void)
{
    return target_hw_h264_api.time_us ? target_hw_h264_api.time_us() :
           (uint32_t)current_tick * (1000000 / HZ);
}

static const struct h264_decode_timing *decoder_timing(
    const struct vpu_h264 *decoder)
{
    return owns_decode(decoder) && target_hw_h264_api.decode_timing ?
        target_hw_h264_api.decode_timing(decoder) : NULL;
}

static int decoder_decode(struct vpu_h264 *decoder, const uint8_t *sample,
                          int length, int nalu_len_size)
{
    return owns_decode(decoder) ?
        target_hw_h264_api.decode_sample(decoder, sample, length,
                                         nalu_len_size) : -1;
}

static void decoder_frame(const struct vpu_h264 *decoder,
                          const uint8_t **y, const uint8_t **cb,
                          const uint8_t **cr, int *w, int *h, int *stride)
{
    if (owns_decode(decoder))
        target_hw_h264_api.get_frame(decoder, y, cb, cr, w, h, stride);
}

static void decoder_close(struct vpu_h264 *decoder)
{
    if (owns(decoder))
    {
        target_hw_h264_api.close(decoder);
        active = NULL;
        decode_worker = 0;
    }
}

const struct hw_h264_api rock_h264_api = {
    .buf_size = decoder_size,
    .open = decoder_open,
    .configure = decoder_configure,
    .decode_sample = decoder_decode,
    .get_frame = decoder_frame,
    .close = decoder_close,
    .time_us = rock_time_us,
    .decode_timing = decoder_timing,
#if defined(IPOD_6G) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
    .present = lcd_blit_video_timed,
#endif
    .set_decode_worker = set_decode_worker,
};
