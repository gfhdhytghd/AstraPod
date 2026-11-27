/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025-2026 David Cormier
 *
 * MP4 video demuxer - extracts H.264 video track data from MP4/M4V files.
 * Based on libm4a/demux.c box-walking pattern, with fd-based I/O and
 * video track support (vmhd + avc1/avcC parsing).
 *
 * Original demux.c copyright:
 * Copyright (c) 2005 David Hammerton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "plugin.h"
#include "system.h"
#include "file.h"
#include "rbendian.h"
#include "mp4_demux.h"

#include <string.h>

/* --- Simple fd-based stream I/O --- */

struct mp4_stream {
    int fd;
    off_t pos;
    off_t size;
    bool eof;
};

static void stream_init(struct mp4_stream *s, int fd, off_t size)
{
    s->fd = fd;
    s->pos = 0;
    s->size = size;
    s->eof = false;
}

static bool stream_read(struct mp4_stream *s, void *buf, size_t len)
{
    size_t done = 0;

    if (s->pos < 0 || s->pos > s->size ||
        len > (size_t)(s->size - s->pos))
    {
        s->eof = true;
        return false;
    }

    while (done < len)
    {
        ssize_t count = rb->read(s->fd, (uint8_t *)buf + done, len - done);

        if (count <= 0)
        {
            s->eof = true;
            return false;
        }
        done += (size_t)count;
        s->pos += count;
    }
    return true;
}

static uint32_t stream_read_uint32(struct mp4_stream *s)
{
    uint8_t buf[4];
    if (!stream_read(s, buf, 4))
        return 0;
    /* big-endian */
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
}

static uint16_t stream_read_uint16(struct mp4_stream *s)
{
    uint8_t buf[2];
    if (!stream_read(s, buf, 2))
        return 0;
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

static uint8_t stream_read_uint8(struct mp4_stream *s)
{
    uint8_t b;
    if (!stream_read(s, &b, 1))
        return 0;
    return b;
}

static bool stream_read_uint32_array(struct mp4_stream *s, uint32_t *values,
                                     uint32_t count)
{
    uint32_t i;

    if (!stream_read(s, values, (size_t)count * sizeof(*values)))
        return false;
    for (i = 0; i < count; i++)
        values[i] = betoh32(values[i]);
    return true;
}

static void stream_skip(struct mp4_stream *s, size_t n)
{
    if (s->pos < 0 || s->pos > s->size ||
        n > (size_t)(s->size - s->pos))
    {
        s->eof = true;
        return;
    }
    s->pos = rb->lseek(s->fd, (off_t)n, SEEK_CUR);
    if (s->pos < 0)
        s->eof = true;
}

static off_t stream_tell(struct mp4_stream *s)
{
    return s->pos;
}

/* --- Box parsers --- */

struct mp4_parse_ctx {
    struct mp4_stream stream;
    struct mp4v_demux_res *res;
    bool found_video;    /* set when we find a video track */
    bool found_audio;    /* set when we find an audio track */
    bool in_video_trak;  /* currently inside a video trak */
    bool in_audio_trak;  /* currently inside an audio trak */
    uint16_t tkhd_dw;    /* tkhd display width (pending video confirm) */
    uint16_t tkhd_dh;    /* tkhd display height (pending video confirm) */
    /* Pending per-trak fields (committed at end of trak once type is known) */
    uint32_t pending_timescale;   /* from mdhd (parsed before hdlr) */
    uint32_t pending_lead_trim;   /* from edts/elst (parsed before mdia) */
};

/* Parse avcC box - H.264 decoder configuration record.
 * Layout: configurationVersion(1) + AVCProfileIndication(1) +
 *         profile_compatibility(1) + AVCLevelIndication(1) +
 *         lengthSizeMinusOne(1, lower 2 bits) +
 *         numSPS(1, lower 5 bits) + [spsLen(2)+spsData]... +
 *         numPPS(1) + [ppsLen(2)+ppsData]... */
static bool read_chunk_avcc(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t data_len;
    uint8_t config_ver;

    if (chunk_len < 8)
        return false;
    data_len = chunk_len - 8;
    if (data_len > MP4V_MAX_CODECDATA || data_len < 7)
        return false;

    /* Read the entire avcC payload into codecdata */
    ctx->res->codecdata_len = data_len;
    if (!stream_read(&ctx->stream, ctx->res->codecdata, data_len))
        return false;

    /* Parse key fields from the buffer */
    config_ver = ctx->res->codecdata[0];
    if (config_ver != 1)
        return false;

    ctx->res->avc_profile   = ctx->res->codecdata[1];
    ctx->res->avc_level     = ctx->res->codecdata[3];
    ctx->res->nalu_len_size = (ctx->res->codecdata[4] & 0x03) + 1;
    ctx->res->num_sps       = ctx->res->codecdata[5] & 0x1F;

    /* Count PPS: skip past SPS entries to find numPPS */
    {
        size_t offset = 6;
        uint8_t i;

        for (i = 0; i < ctx->res->num_sps && offset + 2 <= data_len; i++)
        {
            uint16_t sps_len = ((uint16_t)ctx->res->codecdata[offset] << 8)
                             | ctx->res->codecdata[offset + 1];
            offset += 2 + sps_len;
        }

        if (offset < data_len)
            ctx->res->num_pps = ctx->res->codecdata[offset];
    }

    return true;
}

/* Parse stsd for video: expects avc1 sample entry */
static bool read_chunk_stsd_video(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t numentries, i;

    if (chunk_len < 16)
        return false;
    size_remaining = chunk_len - 8;
    /* version + flags */
    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    for (i = 0; i < numentries && size_remaining > 8; i++)
    {
        uint32_t entry_size = stream_read_uint32(&ctx->stream);
        uint32_t format = stream_read_uint32(&ctx->stream);
        uint32_t entry_remaining = entry_size - 8;

        if (entry_size <= 8 || entry_size > size_remaining)
            return false;

        if (format == MAKEFOURCC('a','v','c','1')
            || format == MAKEFOURCC('a','v','c','3'))
        {
            if (entry_remaining < 78)
                return false;
            ctx->res->format = format;

            /* reserved(6) + data_ref_index(2) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* pre-defined(2) + reserved(2) + pre-defined(12) */
            stream_skip(&ctx->stream, 16);
            entry_remaining -= 16;

            ctx->res->width = stream_read_uint16(&ctx->stream);
            ctx->res->height = stream_read_uint16(&ctx->stream);
            entry_remaining -= 4;

            /* horiz_resolution(4) + vert_resolution(4) + reserved(4) +
               frame_count(2) + compressor_name(32) + depth(2) +
               pre_defined(2) */
            stream_skip(&ctx->stream, 50);
            entry_remaining -= 50;

            /* Walk sub-boxes looking for avcC */
            while (entry_remaining > 8)
            {
                uint32_t sub_len = stream_read_uint32(&ctx->stream);
                uint32_t sub_id  = stream_read_uint32(&ctx->stream);

                if (sub_len <= 8 || sub_len > entry_remaining)
                    break;

                if (sub_id == MAKEFOURCC('a','v','c','C'))
                {
                    if (!read_chunk_avcc(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }

                entry_remaining -= sub_len;
            }

            if (entry_remaining > 0)
                stream_skip(&ctx->stream, entry_remaining);
        }
        else
        {
            /* Unknown video format, skip */
            stream_skip(&ctx->stream, entry_remaining);
        }

        size_remaining -= entry_size;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return ctx->res->format != 0;
}

/* Read MPEG-4 descriptor length (variable length, 7 bits per byte,
 * high bit means continue).
 * Ported from libm4a/demux.c:78-92. */
static uint32_t read_mp4_descr_length(struct mp4_stream *s)
{
    uint8_t b;
    uint8_t num_bytes = 0;
    uint32_t length = 0;

    do {
        b = stream_read_uint8(s);
        num_bytes++;
        length = (length << 7) | (b & 0x7F);
    } while ((b & 0x80) && num_bytes < 4);

    return length;
}

/* Parse esds box - extract AudioSpecificConfig for AAC decoder init.
 * Ported from libm4a/demux.c:95-154. */
static bool read_chunk_esds(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    off_t box_end;
    uint8_t tag;
    uint32_t descr_len;

    if (chunk_len < 12)
        return false;
    box_end = ctx->stream.pos + (off_t)(chunk_len - 8);

    /* version + flags */
    stream_read_uint32(&ctx->stream);

    /* ES_Descriptor (tag 0x03) */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);
    (void)descr_len;

    if (tag == 0x03)
    {
        /* ES_ID(2) + streamPriority(1) */
        stream_skip(&ctx->stream, 3);
    }
    else
    {
        /* Non-standard: skip 2 bytes instead */
        stream_skip(&ctx->stream, 2);
    }

    /* DecoderConfigDescriptor (tag 0x04) */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);
    (void)descr_len;

    if (tag != 0x04)
    {
        off_t remaining = box_end - ctx->stream.pos;

        if (remaining < 0)
            return false;
        stream_skip(&ctx->stream, (size_t)remaining);
        return !ctx->stream.eof;
    }

    /* objectTypeIndication(1) + streamType(1) + bufferSizeDB(3) +
       maxBitrate(4) + avgBitrate(4) = 13 bytes */
    stream_skip(&ctx->stream, 13);

    /* DecoderSpecificInfo (tag 0x05) = AudioSpecificConfig */
    tag = stream_read_uint8(&ctx->stream);
    descr_len = read_mp4_descr_length(&ctx->stream);

    if (tag == 0x05 && descr_len > 0)
    {
        if (descr_len > sizeof(ctx->res->audio_codecdata))
            descr_len = sizeof(ctx->res->audio_codecdata);

        ctx->res->audio_codecdata_len = descr_len;
        stream_read(&ctx->stream, ctx->res->audio_codecdata, descr_len);
    }

    /* Skip any trailing descriptors (SLConfigDescriptor etc.) */
    {
        off_t remaining = box_end - ctx->stream.pos;
        if (remaining > 0)
            stream_skip(&ctx->stream, (size_t)remaining);
    }

    return !ctx->stream.eof && ctx->stream.pos == box_end;
}

/* Parse stsd for audio: expects mp4a sample entry.
 * mp4a layout: reserved(6) + data_ref_index(2) + reserved(8) +
 *   channels(2) + sample_size(2) + packet_size(2) + sample_rate(4, 16.16 FP) +
 *   reserved(2) + sub-boxes (esds, etc.) */
static bool read_chunk_stsd_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t numentries, i;

    if (chunk_len < 16)
        return false;
    size_remaining = chunk_len - 8;
    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    for (i = 0; i < numentries && size_remaining > 8; i++)
    {
        uint32_t entry_size = stream_read_uint32(&ctx->stream);
        uint32_t format = stream_read_uint32(&ctx->stream);
        uint32_t entry_remaining = entry_size - 8;

        if (entry_size <= 8 || entry_size > size_remaining)
            return false;

        if (format == MAKEFOURCC('m','p','4','a'))
        {
            if (entry_remaining < 28)
                return false;
            ctx->res->audio_format = format;

            /* reserved(6) + data_ref_index(2) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* reserved(8) */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* channelCount(2) + sampleSize(2) + compressionID(2) +
               packetSize(2) = 8 bytes */
            stream_skip(&ctx->stream, 8);
            entry_remaining -= 8;

            /* sampleRate: 16.16 fixed-point, 4 bytes */
            {
                uint32_t sr_fp = stream_read_uint32(&ctx->stream);
                ctx->res->audio_sample_rate = sr_fp >> 16;
            }
            entry_remaining -= 4;

            /* Walk sub-boxes for esds */
            while (entry_remaining > 8)
            {
                uint32_t sub_len = stream_read_uint32(&ctx->stream);
                uint32_t sub_id  = stream_read_uint32(&ctx->stream);

                if (sub_len <= 8 || sub_len > entry_remaining)
                    break;

                if (sub_id == MAKEFOURCC('e','s','d','s'))
                {
                    if (!read_chunk_esds(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }

                entry_remaining -= sub_len;
            }

            if (entry_remaining > 0)
                stream_skip(&ctx->stream, entry_remaining);
        }
        else
        {
            stream_skip(&ctx->stream, entry_remaining);
        }

        size_remaining -= entry_size;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

static bool read_chunk_stsz_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t default_size;
    uint32_t numsizes;

    if (chunk_len < 20)
        return false;
    size_remaining = chunk_len - 8;

    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    default_size = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    numsizes = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (default_size == 0 && numsizes > size_remaining / 4)
        return false;
    ctx->res->audio_num_samples = numsizes;
    stream_skip(&ctx->stream, size_remaining);
    return !ctx->stream.eof;
}

static bool read_audio_table_count(struct mp4_parse_ctx *ctx,
                                   size_t chunk_len, size_t entry_size,
                                   uint32_t *count)
{
    size_t size_remaining;
    uint32_t numentries;

    if (chunk_len < 16)
        return false;
    size_remaining = chunk_len - 8;
    stream_read_uint32(&ctx->stream);
    size_remaining -= 4;
    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;
    if (entry_size == 0 || numentries > size_remaining / entry_size)
        return false;
    *count = numentries;
    stream_skip(&ctx->stream, size_remaining);
    return !ctx->stream.eof;
}

static bool read_chunk_stsc_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    return read_audio_table_count(ctx, chunk_len, 12,
                                  &ctx->res->audio_num_stsc);
}

static bool read_chunk_stco_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    return read_audio_table_count(ctx, chunk_len, 4,
                                  &ctx->res->audio_num_stco);
}

static bool read_chunk_co64_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    return read_audio_table_count(ctx, chunk_len, 8,
                                  &ctx->res->audio_num_stco);
}

/* Audio sample table container */
static bool read_chunk_stbl_audio(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('s','t','s','d'):
                if (!read_chunk_stsd_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','z'):
                if (!read_chunk_stsz_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','c'):
                if (!read_chunk_stsc_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','c','o'):
                if (!read_chunk_stco_audio(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('c','o','6','4'):
                if (!read_chunk_co64_audio(ctx, sub_len))
                    return false;
                break;
            /* No stss for audio; all AAC frames are sync samples */
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* stts - time-to-sample table */
static bool read_chunk_stts(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t numentries, i;

    if (chunk_len < 16)
        return false;
    size_remaining = chunk_len - 8;
    stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    numentries = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    if (numentries > MP4V_MAX_STTS || numentries > size_remaining / 8)
        return false;

    ctx->res->num_stts = numentries;
    if (!stream_read(&ctx->stream, ctx->res->stts,
                     (size_t)numentries * sizeof(ctx->res->stts[0])))
        return false;
    size_remaining -= (size_t)numentries * 8;
    for (i = 0; i < numentries; i++)
    {
        ctx->res->stts[i].sample_count =
            betoh32(ctx->res->stts[i].sample_count);
        ctx->res->stts[i].sample_delta =
            betoh32(ctx->res->stts[i].sample_delta);
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* stsz - sample sizes */
static bool read_table(struct mp4_parse_ctx *ctx, struct mp4v_table *table,
                       uint32_t count, unsigned words_per_entry,
                       size_t remaining)
{
    if (count > remaining / (words_per_entry * sizeof(uint32_t)))
        return false;
    table->offset = stream_tell(&ctx->stream);
    table->words = count * words_per_entry;
    stream_skip(&ctx->stream, remaining);
    return !ctx->stream.eof;
}

static bool read_chunk_stsz(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    if (chunk_len < 20)
        return false;
    stream_read_uint32(&ctx->stream); /* version + flags */
    ctx->res->default_sample_size = stream_read_uint32(&ctx->stream);
    ctx->res->num_samples = stream_read_uint32(&ctx->stream);
    return read_table(ctx, &ctx->res->sample_sizes,
                      ctx->res->default_sample_size ? 0 : ctx->res->num_samples,
                      1, chunk_len - 20);
}

static bool read_counted_table(struct mp4_parse_ctx *ctx,
                               struct mp4v_table *table, uint32_t *count,
                               unsigned words_per_entry, size_t chunk_len)
{
    if (chunk_len < 16)
        return false;
    stream_read_uint32(&ctx->stream); /* version + flags */
    *count = stream_read_uint32(&ctx->stream);
    return read_table(ctx, table, *count, words_per_entry, chunk_len - 16);
}

static bool read_chunk_stsc(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    return read_counted_table(ctx, &ctx->res->stsc, &ctx->res->num_stsc,
                              3, chunk_len);
}

static bool read_chunk_stco(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    ctx->res->chunk_words = 1;
    return read_counted_table(ctx, &ctx->res->chunk_offsets,
                              &ctx->res->num_stco, 1, chunk_len);
}

static bool read_chunk_co64(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    ctx->res->chunk_words = 2;
    return read_counted_table(ctx, &ctx->res->chunk_offsets,
                              &ctx->res->num_stco, 2, chunk_len);
}

static bool read_chunk_stss(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    return read_counted_table(ctx, &ctx->res->sync_samples, &ctx->res->num_stss,
                              1, chunk_len);
}

/* stbl - sample table container */
static bool read_chunk_stbl(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('s','t','s','d'):
                if (!read_chunk_stsd_video(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','t','s'):
                if (!read_chunk_stts(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','z'):
                if (!read_chunk_stsz(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','c'):
                if (!read_chunk_stsc(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','c','o'):
                if (!read_chunk_stco(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('c','o','6','4'):
                if (!read_chunk_co64(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('s','t','s','s'):
                if (!read_chunk_stss(ctx, sub_len))
                    return false;
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* minf - accepts vmhd (video) instead of smhd (audio) */
static bool read_chunk_minf(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    /* Walk sub-boxes, look for vmhd to confirm video, then find stbl */
    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('v','m','h','d'):
                /* Video media header confirms this is a video track */
                ctx->in_video_trak = true;
                stream_skip(&ctx->stream, sub_len - 8);
                break;
            case MAKEFOURCC('s','m','h','d'):
                /* Sound media header confirms this is an audio track */
                ctx->in_video_trak = false;
                ctx->in_audio_trak = true;
                stream_skip(&ctx->stream, sub_len - 8);
                break;
            case MAKEFOURCC('s','t','b','l'):
                if (ctx->in_video_trak)
                {
                    if (!read_chunk_stbl(ctx, sub_len))
                        return false;
                }
                else if (ctx->in_audio_trak)
                {
                    if (!read_chunk_stbl_audio(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* mdhd - media header (extract timescale) */
static bool read_chunk_mdhd(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t version;

    if (chunk_len < 28)
        return false;
    size_remaining = chunk_len - 8;
    version = stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    {
        uint32_t ts;

        if ((version >> 24) == 0)
        {
            /* Version 0: 4-byte fields */
            stream_skip(&ctx->stream, 4); /* creation_time */
            stream_skip(&ctx->stream, 4); /* modification_time */
            ts = stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 4); /* duration */
            size_remaining -= 16;
        }
        else if ((version >> 24) == 1)
        {
            if (chunk_len < 40)
                return false;
            /* Version 1: 8-byte fields */
            stream_skip(&ctx->stream, 8); /* creation_time */
            stream_skip(&ctx->stream, 8); /* modification_time */
            ts = stream_read_uint32(&ctx->stream);
            stream_skip(&ctx->stream, 8); /* duration */
            size_remaining -= 28;
        }
        else
        {
            return false;
        }

        /* Save pending; commit at end of trak once type is known.
         * mdhd comes before hdlr in mdia, so track type isn't known yet. */
        ctx->pending_timescale = ts;
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* hdlr - handler reference (check for 'vide') */
static bool read_chunk_hdlr(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t handler_type;

    if (chunk_len < 20)
        return false;
    size_remaining = chunk_len - 8;
    stream_read_uint32(&ctx->stream); /* version + flags */
    stream_read_uint32(&ctx->stream); /* pre_defined */
    handler_type = stream_read_uint32(&ctx->stream);
    size_remaining -= 12;

    ctx->in_video_trak = false;
    ctx->in_audio_trak = false;

    if (handler_type == MAKEFOURCC('v','i','d','e'))
        ctx->in_video_trak = true;
    else if (handler_type == MAKEFOURCC('s','o','u','n'))
        ctx->in_audio_trak = true;

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* mdia - media container */
static bool read_chunk_mdia(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('m','d','h','d'):
                if (!read_chunk_mdhd(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('h','d','l','r'):
                if (!read_chunk_hdlr(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('m','i','n','f'):
                if (!read_chunk_minf(ctx, sub_len))
                    return false;
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* tkhd - track header (extract display width/height for PAR) */
static bool read_chunk_tkhd(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t version;
    uint32_t dw_fp, dh_fp; /* 16.16 fixed-point display dimensions */

    if (chunk_len < 92)
        return false;
    size_remaining = chunk_len - 8;
    version = stream_read_uint32(&ctx->stream); /* version + flags */
    size_remaining -= 4;

    if ((version >> 24) == 0)
    {
        /* Version 0: creation(4)+modification(4)+track_id(4)+reserved(4)+
           duration(4) = 20 bytes, then reserved(8)+layer(2)+alt_group(2)+
           volume(2)+reserved(2)+matrix(36)+width(4)+height(4) = 56 bytes */
        stream_skip(&ctx->stream, 20 + 8 + 2 + 2 + 2 + 2 + 36);
        size_remaining -= 20 + 8 + 2 + 2 + 2 + 2 + 36;
    }
    else if ((version >> 24) == 1)
    {
        if (chunk_len < 104)
            return false;
        /* Version 1: creation(8)+modification(8)+track_id(4)+reserved(4)+
           duration(8) = 32 bytes */
        stream_skip(&ctx->stream, 32 + 8 + 2 + 2 + 2 + 2 + 36);
        size_remaining -= 32 + 8 + 2 + 2 + 2 + 2 + 36;
    }
    else
    {
        return false;
    }

    dw_fp = stream_read_uint32(&ctx->stream); /* width in 16.16 */
    dh_fp = stream_read_uint32(&ctx->stream); /* height in 16.16 */
    size_remaining -= 8;

    /* Commit to res only after mdia confirms video */
    ctx->tkhd_dw = (uint16_t)(dw_fp >> 16);
    ctx->tkhd_dh = (uint16_t)(dh_fp >> 16);

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* edts/elst - edit list (extract media_time for encoder priming offset) */
static bool read_chunk_elst(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining;
    uint32_t version_flags;
    uint32_t entry_count, i;

    if (chunk_len < 16)
        return false;
    size_remaining = chunk_len - 8;
    version_flags = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    entry_count = stream_read_uint32(&ctx->stream);
    size_remaining -= 4;

    {
        size_t entry_size = ((version_flags >> 24) == 0) ? 12 : 20;

        if ((version_flags >> 24) > 1 ||
            entry_count > size_remaining / entry_size)
            return false;
        for (i = 0; i < entry_count; i++)
        {
            int32_t media_time;

            if ((version_flags >> 24) == 0)
            {
                stream_skip(&ctx->stream, 4); /* segment_duration */
                media_time = (int32_t)stream_read_uint32(&ctx->stream);
                stream_skip(&ctx->stream, 4); /* media_rate (16.16) */
                size_remaining -= 12;
            }
            else
            {
                stream_skip(&ctx->stream, 8); /* segment_duration (64-bit) */
                stream_skip(&ctx->stream, 4); /* media_time high */
                media_time = (int32_t)stream_read_uint32(&ctx->stream);
                stream_skip(&ctx->stream, 4); /* media_rate */
                size_remaining -= 20;
            }

            /* First non-empty edit: media_time > 0 = priming offset.
             * Save until the track type is known. */
            if (i == 0 && media_time > 0)
                ctx->pending_lead_trim = (uint32_t)media_time;
        }
    }

    if (size_remaining > 0)
        stream_skip(&ctx->stream, size_remaining);

    return true;
}

/* edts container: walk for elst */
static bool read_chunk_edts(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        if (sub_id == MAKEFOURCC('e','l','s','t'))
        {
            if (!read_chunk_elst(ctx, sub_len))
                return false;
        }
        else
        {
            stream_skip(&ctx->stream, sub_len - 8);
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* trak - track container */
static bool read_chunk_trak(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    /* Reset per-track state (pending fields MUST be reset to prevent
     * stale values from one trak leaking into the next) */
    ctx->in_video_trak = false;
    ctx->in_audio_trak = false;
    ctx->pending_timescale = 0;
    ctx->pending_lead_trim = 0;
    ctx->tkhd_dw = 0;
    ctx->tkhd_dh = 0;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('t','k','h','d'):
                if (!read_chunk_tkhd(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('m','d','i','a'):
                if (!read_chunk_mdia(ctx, sub_len))
                    return false;
                break;
            case MAKEFOURCC('e','d','t','s'):
                /* edts comes before mdia, so the track type is unknown.
                 * Parse unconditionally, save to pending_lead_trim.
                 * Committed to audio_lead_trim at end of trak. */
                if (!read_chunk_edts(ctx, sub_len))
                    return false;
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    /* Commit pending per-trak fields now that track type is known.
     * mdhd timescale and edts/elst are parsed before hdlr identifies
     * the track type, so they were saved to pending fields above. */
    if (ctx->in_video_trak && ctx->res->format != 0)
    {
        ctx->found_video = true;
        ctx->res->display_width = ctx->tkhd_dw;
        ctx->res->display_height = ctx->tkhd_dh;
        ctx->res->timescale = ctx->pending_timescale;
    }

    if (ctx->in_audio_trak && ctx->res->audio_format != 0)
    {
        ctx->found_audio = true;
        ctx->res->audio_lead_trim = ctx->pending_lead_trim;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

/* moov - movie container */
static bool read_chunk_moov(struct mp4_parse_ctx *ctx, size_t chunk_len)
{
    size_t size_remaining = chunk_len - 8;

    while (size_remaining >= 8)
    {
        uint32_t sub_len = stream_read_uint32(&ctx->stream);
        uint32_t sub_id  = stream_read_uint32(&ctx->stream);

        if (sub_len < 8 || sub_len > size_remaining)
            return false;

        switch (sub_id)
        {
            case MAKEFOURCC('t','r','a','k'):
                /* Parse until we have both video and audio (or all traks) */
                if (!ctx->found_video || !ctx->found_audio)
                {
                    if (!read_chunk_trak(ctx, sub_len))
                        return false;
                }
                else
                {
                    stream_skip(&ctx->stream, sub_len - 8);
                }
                break;
            default:
                stream_skip(&ctx->stream, sub_len - 8);
                break;
        }

        size_remaining -= sub_len;
    }

    return !ctx->stream.eof && size_remaining == 0;
}

int mp4v_demux_read(int fd, struct mp4v_demux_res *res)
{
    struct mp4_parse_ctx ctx;
    off_t file_size = rb->ffilesize(fd);

    if (file_size <= 0 || (uint64_t)file_size > UINT32_MAX)
        return -1;

    rb->memset(res, 0, sizeof(*res));

    rb->memset(&ctx, 0, sizeof(ctx));
    stream_init(&ctx.stream, fd, file_size);
    ctx.res = res;

    /* Walk top-level boxes */
    while (ctx.stream.pos < file_size && !ctx.stream.eof)
    {
        uint32_t chunk_len = stream_read_uint32(&ctx.stream);
        uint32_t chunk_id;

        if (ctx.stream.eof)
            break;

        if (chunk_len < 8)
        {
            return -1;
        }

        chunk_id = stream_read_uint32(&ctx.stream);
        if (ctx.stream.eof ||
            chunk_len - 8 > (uint32_t)(file_size - ctx.stream.pos))
        {
            return -1;
        }

        switch (chunk_id)
        {
            case MAKEFOURCC('f','t','y','p'):
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
            case MAKEFOURCC('m','o','o','v'):
                if (!read_chunk_moov(&ctx, chunk_len))
                    return -1;
                break;
            case MAKEFOURCC('m','d','a','t'):
                if (chunk_len > 8)
                {
                    res->mdat_offset = (uint32_t)stream_tell(&ctx.stream);
                    res->mdat_len = chunk_len - 8;
                }
                /* If we already found everything we need, we're done */
                if (ctx.found_video && ctx.found_audio)
                    goto done;
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
            default:
                stream_skip(&ctx.stream, chunk_len - 8);
                break;
        }
    }

done:

    if (ctx.stream.eof || !ctx.found_video || res->format == 0)
        return -1;

    return 0;
}

static bool table_word(int fd, struct mp4v_table *table, uint32_t index,
                       uint32_t *value)
{
    if (index >= table->words)
        return false;
    if (index < table->first || index - table->first >= table->count)
    {
        struct mp4_stream stream;
        uint32_t first = index - index % MP4V_WINDOW_WORDS;
        uint32_t count = MIN(MP4V_WINDOW_WORDS, table->words - first);
        off_t pos = (off_t)table->offset + (off_t)first * sizeof(uint32_t);
        size_t bytes = count * sizeof(uint32_t);

        table->count = 0;
        stream_init(&stream, fd, pos + bytes);
        stream.pos = pos;
        if (rb->lseek(fd, pos, SEEK_SET) != pos ||
            !stream_read_uint32_array(&stream, table->window, count))
            return false;
        table->first = first;
        table->count = count;
    }
    *value = table->window[index - table->first];
    return true;
}

static bool chunk_run(int fd, struct mp4v_demux_res *res, uint32_t run,
                      uint32_t *first, uint32_t *end, uint32_t *per_chunk)
{
    *end = res->num_stco + 1;
    return table_word(fd, &res->stsc, 3 * run, first) &&
           table_word(fd, &res->stsc, 3 * run + 1, per_chunk) &&
           (run + 1 == res->num_stsc ||
            table_word(fd, &res->stsc, 3 * (run + 1), end)) &&
           *first > 0 && *end > *first && *end <= res->num_stco + 1 &&
           *per_chunk > 0;
}

bool mp4v_validate_index(int fd, struct mp4v_demux_res *res)
{
    uint64_t samples = 0;
    uint32_t previous = 0;

    for (uint32_t i = 0; i < res->num_stsc; i++)
    {
        uint32_t first, end, per_chunk, description;
        if (!chunk_run(fd, res, i, &first, &end, &per_chunk) ||
            (i == 0 && first != 1) ||
            !table_word(fd, &res->stsc, 3 * i + 2, &description) ||
            !description)
            return false;
        samples += (uint64_t)(end - first) * per_chunk;
        if (samples > res->num_samples)
            return false;
    }

    for (uint32_t i = 0; i < res->num_stss; i++)
    {
        uint32_t sample;
        if (!table_word(fd, &res->sync_samples, i, &sample) ||
            sample <= previous || sample > res->num_samples)
            return false;
        previous = sample;
    }
    return samples == res->num_samples;
}

static bool sample_size(int fd, struct mp4v_demux_res *res, uint32_t sample,
                        uint32_t *size)
{
    *size = res->default_sample_size;
    return *size || table_word(fd, &res->sample_sizes, sample, size);
}

int mp4v_get_sample_offset(int fd, struct mp4v_demux_res *res,
                           uint32_t sample_index,
                           uint32_t *offset_out, uint32_t *size_out)
{
    uint64_t offset = res->next_offset;
    if (sample_index >= res->num_samples ||
        !sample_size(fd, res, sample_index, size_out))
        return -1;

    if (sample_index != res->next_sample ||
        sample_index >= res->chunk_end_sample)
    {
        uint32_t run = res->map_run;
        uint32_t sample = res->map_run_sample;
        uint32_t first, end, per_chunk;
        uint32_t low, high = 0;

        if (sample_index < sample)
            run = sample = 0;
        for (;;)
        {
            if (run >= res->num_stsc ||
                !chunk_run(fd, res, run, &first, &end, &per_chunk))
                return -1;
            uint64_t count = (uint64_t)(end - first) * per_chunk;
            if (sample_index - sample < count)
                break;
            sample += count;
            run++;
        }

        uint32_t in_run = sample_index - sample;
        uint32_t chunk = first - 1 + in_run / per_chunk;
        uint32_t word = chunk * res->chunk_words;
        if ((res->chunk_words == 2 &&
             !table_word(fd, &res->chunk_offsets, word++, &high)) ||
            high || !table_word(fd, &res->chunk_offsets, word, &low))
            return -1;

        uint32_t in_chunk = in_run % per_chunk;
        offset = low;
        if (res->default_sample_size)
            offset += (uint64_t)in_chunk * res->default_sample_size;
        else
        {
            for (uint32_t i = sample_index - in_chunk; i < sample_index; i++)
            {
                uint32_t size;
                if (!sample_size(fd, res, i, &size))
                    return -1;
                offset += size;
            }
        }
        res->map_run = run;
        res->map_run_sample = sample;
        res->chunk_end_sample = sample_index - in_chunk + per_chunk;
    }

    if (offset < res->mdat_offset ||
        offset + *size_out > (uint64_t)res->mdat_offset + res->mdat_len)
        return -1;
    *offset_out = (uint32_t)offset;
    res->next_offset = offset + *size_out;
    res->next_sample = sample_index + 1;
    return 0;
}

bool mp4v_find_keyframe(int fd, struct mp4v_demux_res *res,
                       uint32_t sample, uint32_t *keyframe)
{
    uint32_t low = 0, high = res->num_stss;
    *keyframe = high ? 0 : sample;
    while (low < high)
    {
        uint32_t mid = low + (high - low) / 2;
        uint32_t sync;
        if (!table_word(fd, &res->sync_samples, mid, &sync))
            return false;
        if (sync - 1 > sample)
            high = mid;
        else
        {
            *keyframe = sync - 1;
            low = mid + 1;
        }
    }
    return true;
}
