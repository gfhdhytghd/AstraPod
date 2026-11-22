/* Bounded line input with byte offsets. GPL version 2 or later. */
struct line_reader {
    off_t size;
    off_t position;
    int fd;
    uint32_t digest;
    unsigned int remaining;
    const unsigned char *next;
    unsigned char block[1024];
};

static int line_byte(struct line_reader *reader)
{
    if (reader->position == reader->size)
        return -1;
    if (!reader->remaining)
    {
        size_t count = MIN((off_t)sizeof(reader->block),
                           reader->size - reader->position);
        if (rb->read(reader->fd, reader->block, count) != (ssize_t)count)
            return -2;
        reader->remaining = count;
        reader->next = reader->block;
    }
    reader->position++;
    reader->remaining--;
    unsigned char byte = *reader->next++;
    reader->digest = (reader->digest ^ byte) * 16777619u;
    return byte;
}

/* Returns one for a line, zero for EOF and a negative value on error.
 * UTF-16 input remains in its original byte order. */
static int input_line(struct line_reader *reader, unsigned char *line,
                      size_t capacity, size_t *used, int encoding_width,
                      bool little_endian)
{
    *used = 0;
    bool cr = false;
    while (true)
    {
        int a = line_byte(reader);
        if (a < 0)
            return a == -1 && !cr ? (*used != 0) : -1;

        int b = 0;
        if (encoding_width == 2)
        {
            b = line_byte(reader);
            if (b < 0)
                return -1;
        }

        int c = little_endian ? (a | b << 8) : (a << 8 | b);
        if (encoding_width == 1)
            c = a;

        if (c == '\n')
            return 1;
        if (!c || cr || *used + encoding_width >= capacity)
            return -1;
        if (c == '\r')
        {
            cr = true;
            continue;
        }

        line[(*used)++] = a;
        if (encoding_width == 2)
            line[(*used)++] = b;
    }
}
