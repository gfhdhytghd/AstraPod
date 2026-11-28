/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Dave Chapman
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

#include "codeclib.h"
#include "libm4a/m4a.h"
#include "libalac/decomp.h"

#ifdef HAVE_SM1_ALAC_API
#include "alac_packet.h"

/* Fetch stsz in groups without resetting the streaming audio handle. */
struct alac_sizes {
    unsigned char bytes[128 * 4];
    uint32_t first, count;
};

static unsigned alac_size(struct alac_sizes *cache, const demux_res_t *demux,
                          uint32_t index)
{
    if (index < cache->first || index - cache->first >= cache->count)
    {
        uint64_t pos = (uint64_t)(uint32_t)demux->sample_byte_sizes_offset
                       + 4ULL * index;
        unsigned count = demux->num_sample_byte_sizes - index;
        if (count > 128)
            count = 128;
        size_t size = count * 4;
        if (ci->filesize < 0 || pos + size > (uint64_t)ci->filesize)
            return 0;

        if (ci->audio_hid >= 0)
        {
            if (!ci->alac_read_at ||
                ci->alac_read_at(pos, cache->bytes, size) != size)
                return 0;
        }
        else
        {
            /* The offline converter owns its input stream. */
            off_t saved = ci->curpos;
            if (!ci->seek_buffer(pos))
                return 0;
            size_t got = ci->read_filebuf(cache->bytes, size);
            if (!ci->seek_buffer(saved) || got != size)
                return 0;
        }
        cache->first = index;
        cache->count = count;
    }
    unsigned char *p = cache->bytes + (index - cache->first) * 4;
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}
#endif

CODEC_HEADER

/* The maximum buffer size handled. This amount of bytes is buffered for each
 * frame. */
#define ALAC_BYTE_BUFFER_SIZE 32768

static int32_t outputbuffer[ALAC_MAX_CHANNELS][ALAC_BLOCKSIZE] IBSS_ATTR;

static void set_elapsed_samples(uint32_t samplesdone)
{
    uint32_t elapsedtime = (uint64_t)samplesdone * 1000ULL / ci->id3->frequency;
    ci->set_elapsed(elapsedtime);
}

/* this is the codec entry point */
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Generic codec initialisation */
        ci->configure(DSP_SET_STEREO_MODE, STEREO_NONINTERLEAVED);
        ci->configure(DSP_SET_SAMPLE_DEPTH, ALAC_OUTPUT_DEPTH-1);
    }

    return CODEC_OK;
}

/* this is called for each file to process */
enum codec_status codec_run(void)
{
    size_t n;
    demux_res_t demux_res;
    stream_t input_stream;
    uint64_t samplesdone;
    int samplesdecoded;
    uint32_t i;
    unsigned char* buffer;
    alac_file alac;
    intptr_t param;
    unsigned long resume_time;
    uint32_t resume_offset;
    unsigned int did_resume;
    uint32_t lookup_table_idx = 0;

    /* Clean and initialize decoder structures */
    memset(&demux_res , 0, sizeof(demux_res));
    if (codec_init()) {
        LOGF("ALAC: Error initialising codec\n");
        return CODEC_ERROR;
    }

    ci->configure(DSP_SET_FREQUENCY, ci->id3->frequency);
    codec_set_replaygain(ci->id3);

    ci->seek_buffer(0);

    stream_create(&input_stream,ci);

    /* Save resume info because qtmovie_read() can modify it. */
    resume_time = ci->id3->elapsed;
    resume_offset = ci->id3->offset;

    /* if qtmovie_read returns successfully, the stream is up to
     * the movie data, which can be used directly by the decoder */
    if (!qtmovie_read(&input_stream, &demux_res)) {
        LOGF("ALAC: Error initialising file\n");
        return CODEC_ERROR;
    }

    /* initialise the sound converter */
    alac_set_info(&alac, demux_res.codecdata);

    if (resume_time)
        did_resume = m4a_seek(&demux_res, &input_stream,
                              (uint64_t)resume_time * ci->id3->frequency / 1000ULL,
                              &samplesdone, &i, &lookup_table_idx);
    else if (resume_offset)
        did_resume = m4a_seek_raw(&demux_res, &input_stream, resume_offset,
                                  &samplesdone, &i, &lookup_table_idx);
    else
        did_resume = 0;

    /* Start from the beginning if we did not resume. */
    if (!did_resume) {
        i = 0;
        samplesdone = 0;
    }

#ifdef HAVE_SM1_ALAC_API
    struct alac_sizes sizes = { .count = 0 };
    bool accelerated = ci->alac_begin && alac.setinfo_max_samples_per_frame == 4096 &&
        alac.setinfo_7a == 0 && (ci->audio_hid < 0 || ci->id3->frequency <= 48000) &&
        ci->alac_begin(alac.setinfo_sample_size, alac.setinfo_7f,
                       alac.setinfo_rice_historymult, alac.setinfo_rice_kmodifier,
                       alac.setinfo_rice_initialhistory);
#endif
    set_elapsed_samples(samplesdone);

    /* The main decoding loop */
    while (i < demux_res.num_sample_byte_sizes) {
        long action = ci->get_command(&param);

        if (action == CODEC_ACTION_HALT)
            break;

        /* Deal with any pending seek requests */
        if (action == CODEC_ACTION_SEEK_TIME) {
            if (m4a_seek(&demux_res, &input_stream,
                         (uint64_t)param * ci->id3->frequency / 1000ULL,
                         &samplesdone, &i, &lookup_table_idx))
                set_elapsed_samples(samplesdone);

            ci->seek_complete();
            if (i >= demux_res.num_sample_byte_sizes)
                break;
        }

#ifdef HAVE_SM1_ALAC_API
        /* Chunk gaps still apply after a packet falls back to the ARM. */
        int offset = m4a_check_sample_offset(&demux_res, i, &lookup_table_idx);
        if (offset > 0 && ci->curpos != offset && !ci->seek_buffer(offset))
        {
            if (accelerated)
                ci->alac_end();
            return CODEC_ERROR;
        }
        if (accelerated)
        {
            off_t packet_start = ci->curpos;
            unsigned bytes = alac_size(&sizes, &demux_res, i);
            if (ci->curpos != packet_start)
            {
                ci->alac_end();
                return CODEC_ERROR;
            }
            if (bytes && bytes <= ALAC_BYTE_BUFFER_SIZE)
            {
                buffer = ci->request_buffer(&n, bytes);
                unsigned count = buffer && n >= bytes ?
                    alac_packet_samples(buffer, bytes, alac.setinfo_7f, alac.setinfo_sample_size) : 0;
                if (count && ci->alac_frame(buffer, bytes, count, outputbuffer[0], outputbuffer[1]))
                {
                    ci->advance_buffer(bytes);
                    ci->pcmbuf_insert(outputbuffer[0], outputbuffer[1], count);
                    samplesdone += count;
                    set_elapsed_samples(samplesdone);
                    ci->yield();
                    i++;
                    continue;
                }
            }
            /* No PCM was committed for this packet. Retry it on the ARM. */
            ci->alac_end();
            accelerated = false;
        }
#endif
        /* Request the required number of bytes from the input buffer */
        buffer=ci->request_buffer(&n, ALAC_BYTE_BUFFER_SIZE);

        /* Decode one block - returned samples will be host-endian */
        samplesdecoded=alac_decode_frame(&alac, buffer, outputbuffer, ci->yield);
        ci->yield();

        /* A frame it cannot decode leaves bytes_consumed unset */
        if (samplesdecoded < 0) {
            LOGF("ALAC: Error decoding frame\n");
            return CODEC_ERROR;
        }

        /* Advance codec buffer by amount of consumed bytes */
        ci->advance_buffer(alac.bytes_consumed);

        /* Output the audio */
        ci->pcmbuf_insert(outputbuffer[0], outputbuffer[1], samplesdecoded);

        /* Update the elapsed-time indicator */
        samplesdone+=samplesdecoded;
        set_elapsed_samples(samplesdone);

        i++;
    }

#ifdef HAVE_SM1_ALAC_API
    if (accelerated)
        ci->alac_end();
#endif
    LOGF("ALAC: Decoded %lu samples\n",(unsigned long)samplesdone);
    return CODEC_OK;
}
