/* AstraPod scratch and file bindings for VBRFix. GPL version 2 or later. */
#include "encoder-file.h"

#define COPY_SIZE 32768
#define XING_TOC_SIZE 100
#define ENCODER_TAG_SIZE 36

struct mpeg_stream {
    struct encoder_file *file;
    unsigned char *scratch;
    off_t first;
    off_t end;
    unsigned long header;
    uint32_t frames;
    bool variable;
    bool cancelled;
};

static unsigned long frame_word(const unsigned char *data)
{
    return rock_mp3_bytes2int(data[0], data[1], data[2], data[3]);
}

static void store_word(unsigned char *data, uint32_t word)
{
    data[0] = word >> 24;
    data[1] = word >> 16;
    data[2] = word >> 8;
    data[3] = word;
}

static bool read_at(int fd, off_t offset, void *data, size_t size)
{
    return rb->lseek(fd, offset, SEEK_SET) == offset &&
           rb->read(fd, data, size) == (ssize_t)size;
}

static int marker_offset(const struct mp3info *frame)
{
    if (frame->version == MPEG_VERSION1)
        return frame->channel_mode == 3 ? 21 : 36;
    return frame->channel_mode == 3 ? 13 : 21;
}

static bool stream_cancel(struct mpeg_stream *stream)
{
    stream->cancelled = encoder_cancel(stream->file);
    return stream->cancelled;
}

/* The second pass records frame starts, using the validated frame total. */
static bool scan_stream(struct mpeg_stream *stream, unsigned char *toc,
                        unsigned int xing_size)
{
    off_t cached_start = 0, cached_end = 0;
    uint32_t count = 0;
    unsigned int next = 1;
    int first_bitrate = 0;
    uint64_t audio_size = stream->end - stream->first;
    for (off_t pos = stream->first; pos < stream->end;)
    {
        if (stream->end - pos < 4)
            return false;
        if (pos + 4 > cached_end)
        {
            if (stream_cancel(stream))
                return false;

            size_t size = MIN((uint64_t)(stream->end - pos), COPY_SIZE);
            if (!read_at(stream->file->input, pos, stream->scratch, size))
                return false;
            cached_start = pos;
            cached_end = pos + size;
            rb->splash_progress((pos - stream->first) * 100LL / audio_size,
                                100, "Scanning MP3\nMenu cancels");
        }

        unsigned long header =
            frame_word(stream->scratch + (pos - cached_start));
        struct mp3info frame;
        if (!is_mp3frameheader(header) || !mp3headerinfo(&frame, header) ||
            frame.layer != 2 || frame.frame_size < 4 ||
            frame.frame_size > stream->end - pos ||
            !headers_have_same_type(stream->header, header))
            return false;
        if (!count)
        {
            stream->header = header;
            first_bitrate = frame.bitrate;
        }
        else if (frame.bitrate != first_bitrate)
            stream->variable = true;
        if (toc)
        {
            while (next < XING_TOC_SIZE && count == next *
                    (uint64_t)stream->frames / XING_TOC_SIZE)
            {
                uint64_t at = pos - stream->first + xing_size;
                toc[next++] = at * 256 / (audio_size + xing_size);
            }
        }
        count++;
        pos += frame.frame_size;
    }

    if (!toc)
        stream->frames = count;
    return count > 1 &&
           (!toc || (count == stream->frames && next == XING_TOC_SIZE));
}

struct previous_tag {
    uint32_t quality;
    bool has_quality;
    bool has_encoder;
    unsigned char encoder[ENCODER_TAG_SIZE];
};

static bool read_previous_tag(struct mpeg_stream *stream, off_t offset,
                              struct previous_tag *previous)
{
    off_t length = stream->first - offset;
    if (!length)
        return true;
    if (length < 4 || length > COPY_SIZE ||
        !read_at(stream->file->input, offset, stream->scratch, length))
        return false;

    unsigned char *data = stream->scratch;
    unsigned long header = frame_word(data);
    struct mp3info frame;
    if (!is_mp3frameheader(header) || !mp3headerinfo(&frame, header) ||
        frame.frame_size != length ||
        !headers_have_same_type(stream->header, header))
        return false;

    unsigned int marker = marker_offset(&frame);
    if (marker + 8 > (unsigned long)length)
        return false;
    if (!rb->memcmp(data + marker, "VBRI", 4))
        return true;
    if (rb->memcmp(data + marker, "Xing", 4) &&
        rb->memcmp(data + marker, "Info", 4))
        return false;

    uint32_t flags = frame_word(data + marker + 4);
    if (flags & ~0xfu)
        return false;
    unsigned int tag = marker + 8;
    if (flags & VBR_FRAMES_FLAG)
        tag += 4;
    if (flags & VBR_BYTES_FLAG)
        tag += 4;
    if (flags & VBR_TOC_FLAG)
        tag += XING_TOC_SIZE;
    if (flags & VBR_QUALITY_FLAG)
    {
        if (tag + 4 > (unsigned long)length)
            return false;
        previous->quality = frame_word(data + tag);
        previous->has_quality = true;
        tag += 4;
    }
    if (tag > (unsigned long)length)
        return false;

    /* Copy the encoder extension, including gapless delay and padding. */
    if (tag + ENCODER_TAG_SIZE <= (unsigned long)length)
    {
        rb->memcpy(previous->encoder, data + tag, ENCODER_TAG_SIZE);
        previous->has_encoder = previous->encoder[0] != 0;
    }
    return true;
}

static unsigned int tag_crc(const unsigned char *data, size_t size)
{
    unsigned int crc = 0;
    for (size_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xa001 : 0);
    }
    return crc;
}

static int make_tag(struct mpeg_stream *stream, unsigned char *output,
                    struct previous_tag *old)
{
    struct mp3info frame;
    unsigned long header = stream->header & ~(BITRATE_MASK | PADDING_MASK);
    header |= (8 << 12) | PROTECTION_MASK;
    if (!mp3headerinfo(&frame, header))
        return 0;
    int marker = marker_offset(&frame);
    int extension = marker + 16 + XING_TOC_SIZE + (old->has_quality ? 4 : 0);
    int required = extension + (old->has_encoder ? ENCODER_TAG_SIZE : 0);
    if (required > frame.frame_size ||
        frame.frame_size > MAX_XING_HEADER_SIZE)
        return 0;

    uint64_t size = (uint64_t)(stream->end - stream->first) + frame.frame_size;
    if (size > UINT32_MAX)
        return 0;
    rb->memset(output, 0, frame.frame_size);
    store_word(output, header);
    rb->memcpy(output + marker, "Xing", 4);
    unsigned int flags = VBR_FRAMES_FLAG | VBR_BYTES_FLAG | VBR_TOC_FLAG;
    if (old->has_quality)
        flags |= VBR_QUALITY_FLAG;
    store_word(output + marker + 4, flags);
    store_word(output + marker + 8, stream->frames);
    store_word(output + marker + 12, size);
    if (!scan_stream(stream, output + marker + 16, frame.frame_size))
        return 0;
    if (old->has_quality)
        store_word(output + extension - 4, old->quality);
    if (old->has_encoder)
    {
        rb->memcpy(output + extension, old->encoder, ENCODER_TAG_SIZE);
        store_word(output + extension + 28, size);
        unsigned int crc = tag_crc(output, extension + ENCODER_TAG_SIZE - 2);
        output[extension + 34] = crc >> 8;
        output[extension + 35] = crc;
    }
    return frame.frame_size;
}

static bool copy_range(struct mpeg_stream *stream, off_t begin, off_t end)
{
    struct encoder_file *file = stream->file;
    for (off_t pos = begin; pos < end;)
    {
        if (stream_cancel(stream))
            return false;
        size_t size = MIN((uint64_t)(end - pos), COPY_SIZE);
        if (!read_at(file->input, pos, stream->scratch, size) ||
            rb->write(file->output, stream->scratch, size) != (ssize_t)size)
            return false;
        pos += size;
        rb->splash_progress((pos - begin) * 100LL / (end - begin), 100,
                            "Writing repaired MP3\nMenu cancels");
    }
    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    if (!path)
    {
        rb->splash(2 * HZ,
                   "Hold Select on an MP3 and choose Repair VBR seek table");
        return PLUGIN_OK;
    }

    const struct mp3entry *playing = rb->audio_current_track();
    if (playing && !rb->strcasecmp(path, playing->path))
    {
        rb->splash(2 * HZ, "Stop this track before repairing it");
        return PLUGIN_OK;
    }
    if (!encoder_confirm("Replace the VBR seek table?"))
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    struct encoder_file file = {.input = -1, .output = -1};
    file.input = rb->open(path, O_RDONLY);
    size_t capacity;
    unsigned char *scratch = rb->plugin_get_buffer(&capacity);
    static struct mp3entry entry;
    off_t size = file.input < 0 ? -1 : rb->ffilesize(file.input);
    if (!scratch || capacity < COPY_SIZE || size <= 0 ||
        !rb->get_metadata(&entry, file.input, path) ||
        entry.codectype != AFMT_MPA_L3 ||
        entry.id3v1len > (uint64_t)size ||
        entry.first_frame_offset >= (uint64_t)size - entry.id3v1len ||
        entry.id3v2len > entry.first_frame_offset)
        return encoder_result(&file, false, false, NULL);

    struct mpeg_stream stream = {
        .file = &file,
        .scratch = scratch,
        .first = entry.first_frame_offset,
        .end = size - entry.id3v1len,
    };
    rb->splash_progress_set_delay(0);
    if (!scan_stream(&stream, NULL, 0))
        return encoder_result(&file, false, stream.cancelled, NULL);
    if (!stream.variable)
    {
        encoder_discard(&file);
        rb->splash(2 * HZ, "Not a variable-bitrate MP3");
        return PLUGIN_OK;
    }

    struct previous_tag previous = {0};
    unsigned char xing[MAX_XING_HEADER_SIZE];
    if (!read_previous_tag(&stream, entry.id3v2len, &previous))
        return encoder_result(&file, false, false, NULL);

    int length = make_tag(&stream, xing, &previous);
    bool ok = length > 0 && encoder_open_output(&file, path) &&
              copy_range(&stream, 0, entry.id3v2len) &&
              rb->write(file.output, xing, length) == length &&
              copy_range(&stream, stream.first, size) &&
              !stream_cancel(&stream) && encoder_commit(&file, path);
    return encoder_result(&file, ok, stream.cancelled,
                          "VBR seek table repaired");
}
