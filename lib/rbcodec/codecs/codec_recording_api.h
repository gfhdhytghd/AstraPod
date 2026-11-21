/* Encoder callbacks from Rockbox codecs.h, GPL-2.0-or-later.
 * Included inside struct codec_api at the target's ABI position. */
    int (*enc_pcmbuf_read)(void *buf, int count);
    int (*enc_pcmbuf_advance)(int count);
    struct enc_chunk_data * (*enc_encbuf_get_buffer)(size_t need);
    void (*enc_encbuf_finish_buffer)(void);
    ssize_t (*enc_stream_read)(void *buf, size_t count);
    off_t (*enc_stream_lseek)(off_t offset, int whence);
    ssize_t (*enc_stream_write)(const void *buf, size_t count);
    int (*round_value_to_list32)(unsigned long value,
                                 const unsigned long list[],
                                 int count,
                                 bool signd);
