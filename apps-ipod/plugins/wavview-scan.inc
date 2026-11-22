/* AstraPod WAV reader and plot scaling. GNU GPL version 2 or later. */
static unsigned int le16(const unsigned char *p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
    return le16(p) | ((uint32_t)le16(p + 2) << 16);
}

struct wave_data {
    off_t offset;
    uint32_t bytes;
    uint32_t rate;
    unsigned int channels;
};

static bool read_wave_header(int fd, struct wave_data *wave)
{
    unsigned char header[12];
    off_t size = rb->ffilesize(fd);
    if (size < (off_t)sizeof(header) ||
        rb->read(fd, header, sizeof(header)) != sizeof(header) ||
        rb->memcmp(header, "RIFF", 4) || rb->memcmp(header + 8, "WAVE", 4))
        return false;

    uint64_t end = (uint64_t)le32(header + 4) + 8;
    if (end < sizeof(header) || end > (uint64_t)size)
        return false;

    for (uint64_t pos = sizeof(header); pos + 8 <= end;)
    {
        unsigned char chunk[8];
        if (rb->lseek(fd, pos, SEEK_SET) != (off_t)pos ||
            rb->read(fd, chunk, sizeof(chunk)) != sizeof(chunk))
            return false;
        uint32_t length = le32(chunk + 4);
        pos += sizeof(chunk);
        if (length > end - pos)
            return false;

        if (!rb->memcmp(chunk, "fmt ", 4))
        {
            unsigned char format[16];
            if (length < sizeof(format) ||
                rb->read(fd, format, sizeof(format)) != sizeof(format))
                return false;
            wave->channels = le16(format + 2);
            wave->rate = le32(format + 4);
            if (le16(format) != 1 || le16(format + 14) != 16 ||
                (wave->channels != 1 && wave->channels != 2) ||
                le16(format + 12) != wave->channels * 2 || wave->rate == 0)
                return false;
        }
        else if (!rb->memcmp(chunk, "data", 4) && wave->channels)
        {
            wave->offset = pos;
            wave->bytes = length;
            return length > 0 && length % (wave->channels * 2) == 0;
        }
        pos += (uint64_t)length + (length & 1);
    }
    return false;
}

enum wave_result {
    WAVE_READY,
    WAVE_INVALID,
    WAVE_CANCELLED,
};

static enum wave_result readwavpeaks(const char *filename)
{
    int fd = rb->open(filename, O_RDONLY);
    if (fd < 0)
        return WAVE_INVALID;

    struct wave_data wave = {0};
    bool ok = read_wave_header(fd, &wave);
    unsigned int frame_size = wave.channels * 2;
    uint32_t frames = ok ? wave.bytes / frame_size : 0;
    struct peakstruct *peaks = (struct peakstruct *)audiobuf;
    uint32_t slots = audiobuflen / sizeof(*peaks);
    if (!ok || !slots || rb->lseek(fd, wave.offset, SEEK_SET) != wave.offset)
    {
        rb->close(fd);
        return WAVE_INVALID;
    }

    const uint32_t per_slot = (frames - 1) / slots + 1;
    unsigned int in_slot = 0;
    for (uint32_t remaining = wave.bytes; remaining;)
    {
        size_t count = MIN(remaining, sizeof(samples));
        if (rb->read(fd, samples, count) != (ssize_t)count)
        {
            ok = false;
            break;
        }
        for (size_t i = 0; i < count; i += frame_size)
        {
            int left = (int16_t)le16(samples + i);
            int right = wave.channels == 2 ? (int16_t)le16(samples + i + 2) : left;
            struct peakstruct *peak = &peaks[mempeakcount];
            if (!in_slot)
                *peak = (struct peakstruct) {left, left, right, right};
            else
            {
                peak->lmin = MIN(peak->lmin, left);
                peak->lmax = MAX(peak->lmax, left);
                peak->rmin = MIN(peak->rmin, right);
                peak->rmax = MAX(peak->rmax, right);
            }
            if (++in_slot == per_slot)
            {
                ++mempeakcount;
                in_slot = 0;
            }
        }
        remaining -= count;
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
        {
            rb->close(fd);
            return WAVE_CANCELLED;
        }
    }
    if (in_slot)
        ++mempeakcount;
    rb->close(fd);
    return ok ? WAVE_READY : WAVE_INVALID;
}

static void displaypeaks(void)
{
    const struct peakstruct *peaks = (const struct peakstruct *)audiobuf;
    uint32_t visible = MAX(1u, mempeakcount / zoomlevel);
    uint32_t start = center > visible / 2 ? center - visible / 2 : 0;
    start = MIN(start, mempeakcount - visible);

    rb->lcd_clear_display();
    unsigned color = rb->lcd_get_foreground();
    rb->lcd_set_foreground(LCD_LIGHTGRAY);
    rb->lcd_hline(0, LCD_WIDTH - 1, LEFTZERO);
    rb->lcd_hline(0, LCD_WIDTH - 1, RIGHTZERO);
    rb->lcd_set_foreground(color);
    int bar_start = start * LCD_WIDTH / mempeakcount;
    int bar_end = (start + visible) * LCD_WIDTH / mempeakcount - 1;
    rb->lcd_hline(bar_start, MAX(bar_start, bar_end), LCD_HEIGHT / 2);

    for (unsigned int x = 0; x < LCD_WIDTH; ++x)
    {
        uint32_t first = start + x * visible / LCD_WIDTH;
        uint32_t end = start + (x + 1) * visible / LCD_WIDTH;
        struct peakstruct column = peaks[first];
        for (uint32_t i = first + 1; i < end; ++i)
        {
            column.lmin = MIN(column.lmin, peaks[i].lmin);
            column.lmax = MAX(column.lmax, peaks[i].lmax);
            column.rmin = MIN(column.rmin, peaks[i].rmin);
            column.rmax = MAX(column.rmax, peaks[i].rmax);
        }
        rb->lcd_vline(x, LEFTZERO - column.lmax / YSCALE,
                     LEFTZERO - column.lmin / YSCALE);
        rb->lcd_vline(x, RIGHTZERO - column.rmax / YSCALE,
                     RIGHTZERO - column.rmin / YSCALE);
    }
    rb->lcd_update();
}
