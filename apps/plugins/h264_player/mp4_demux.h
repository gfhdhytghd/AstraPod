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
 * H.264 player MP4 demuxer - extracts video track data from MP4/M4V files.
 * Forked from libm4a/demux.c, adapted for fd-based I/O and video tracks.
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
#ifndef MP4_DEMUX_H
#define MP4_DEMUX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Max SPS/PPS codec data from avcC box */
#define MP4V_MAX_CODECDATA  512

#define MP4V_MAX_STTS       4096

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) ( \
    ( (int32_t)(char)(ch0) << 24 ) | \
    ( (int32_t)(char)(ch1) << 16 ) | \
    ( (int32_t)(char)(ch2) << 8 ) | \
    ( (int32_t)(char)(ch3) ) )
#endif

struct mp4v_stts_entry {
    uint32_t sample_count;
    uint32_t sample_delta;
};

/* Each table has its own window: reading one must not evict the others. */
#define MP4V_WINDOW_WORDS  256

struct mp4v_table {
    uint32_t offset;
    uint32_t words;
    uint32_t first;
    uint32_t count;
    uint32_t window[MP4V_WINDOW_WORDS];
};

struct mp4v_demux_res {
    /* Video format */
    uint32_t format;         /* fourcc: 'avc1' etc. */
    uint16_t width;          /* coded width from avc1 sample entry */
    uint16_t height;         /* coded height from avc1 sample entry */
    uint16_t display_width;  /* display width from tkhd (PAR-adjusted) */
    uint16_t display_height; /* display height from tkhd */
    uint32_t timescale;      /* from mdhd */

    /* avcC decoder config (SPS + PPS) */
    uint8_t  avc_profile;
    uint8_t  avc_level;
    uint8_t  nalu_len_size;  /* length field size (1-4 bytes, typically 4) */
    uint8_t  num_sps;
    uint8_t  num_pps;
    uint32_t codecdata_len;
    uint8_t  codecdata[MP4V_MAX_CODECDATA]; /* raw avcC data */

    /* Sample sizes (stsz) */
    uint32_t num_samples;
    uint32_t default_sample_size;
    struct mp4v_table sample_sizes;

    /* Time-to-sample (stts) */
    uint32_t num_stts;
    struct mp4v_stts_entry stts[MP4V_MAX_STTS];

    /* Sample-to-chunk (stsc) */
    uint32_t num_stsc;
    struct mp4v_table stsc;

    /* Chunk offsets (stco) */
    uint32_t num_stco;
    uint32_t chunk_words;
    struct mp4v_table chunk_offsets;

    /* Sync samples / keyframes (stss) */
    uint32_t num_stss;
    struct mp4v_table sync_samples;

    /* Mapping cursor for sequential playback. Reset on a backward seek. */
    uint32_t map_run;
    uint32_t map_run_sample;
    uint32_t next_sample;
    uint32_t chunk_end_sample;
    uint64_t next_offset;

    /* File and mdat location */
    uint32_t mdat_offset;
    uint32_t mdat_len;

    /* Audio track (populated when mp4a trak found) */
    uint32_t audio_format;          /* fourcc: 'mp4a' or 0 if no audio */
    uint32_t audio_sample_rate;     /* from mp4a sample entry */
    uint8_t  audio_codecdata[64];   /* AudioSpecificConfig from esds */
    uint32_t audio_codecdata_len;
    uint32_t audio_lead_trim;       /* encoder priming from edts/elst */

    /* Audio table counts used to size aac.codec's workspace. The AAC codec
     * parses its own table data from the file. */
    uint32_t audio_num_samples;
    uint32_t audio_num_stsc;
    uint32_t audio_num_stco;
};

/* Parse the caller-owned fd. The same fd backs the sample table windows. */
int mp4v_demux_read(int fd, struct mp4v_demux_res *res);

/* Validate the chunk mapping and ordered sync sample numbers. */
bool mp4v_validate_index(int fd, struct mp4v_demux_res *res);

/* Get the file offset and size of video sample N (0-based). */
int mp4v_get_sample_offset(int fd, struct mp4v_demux_res *res,
                           uint32_t sample_index,
                           uint32_t *offset_out, uint32_t *size_out);

/* Find the last sync sample at or before sample. */
bool mp4v_find_keyframe(int fd, struct mp4v_demux_res *res,
                       uint32_t sample, uint32_t *keyframe);

#endif /* MP4_DEMUX_H */
