/* RAM use is independent of track duration. Every anchor was reached by
 * walking whole CRC-checked frames, and every frame is checked before DMA. */
struct sm1_cursor { unsigned offset, first, ordinal; };
#define SM1_ANCHORS 256
static struct sm1_cursor sm1_anchors[SM1_ANCHORS];
static unsigned anchor_count, anchor_stride, route_bound, route_maxblock, route_bytes;
static unsigned startup_bytes, seek_scanned;

static void sm1_anchor(struct sm1_cursor c)
{
    if ((anchor_count && c.ordinal <= sm1_anchors[anchor_count-1].ordinal) ||
        c.ordinal % anchor_stride) return;
    if (anchor_count == SM1_ANCHORS) {
        for (unsigned i=0; i < SM1_ANCHORS/2; ++i) sm1_anchors[i]=sm1_anchors[2*i];
        anchor_count /= 2;
        anchor_stride *= 2;
        if (c.ordinal % anchor_stride) return;
    }
    sm1_anchors[anchor_count++] = c;
}

static unsigned sm1_validate_view(const uint8_t *data, unsigned n, struct sm1_cursor c,
                                  struct sm1_header *h)
{
    if (route_mp4) {
        if (c.ordinal>=route_packet_count) return 0;
        const struct sm4_packet *packet=&route_packets[c.ordinal];
        if (n!=packet->size || c.offset!=packet->offset || c.first!=packet->sample ||
            !sm1_flac_header(data,n,route_rate,route_depth,h) ||
            h->block!=packet->duration || h->block>route_maxblock ||
            h->number!=(h->variable?c.first:c.ordinal)) return 0;
        uint16_t crc=0;
        for (unsigned i=0;i<n;++i) crc=sm1_crc16_step(crc,data[i]);
        return crc ? 0 : n;
    }
    unsigned bytes = sm1_frame_length(data,n,route_bound,route_rate,route_depth,c.ordinal,c.first,
                                      route_total,n == route_bytes - c.offset,h);
    return bytes && h->block <= route_maxblock ? bytes : 0;
}

static unsigned sm1_file_frame(int fd, struct sm1_cursor c, struct sm1_header *h)
{
    if (c.offset >= route_bytes) return 0;
    if (route_mp4 && c.ordinal>=route_packet_count) return 0;
    unsigned n=route_mp4?route_packets[c.ordinal].size:MIN(route_bytes-c.offset,route_bound+32);
    if (lseek(fd,c.offset,SEEK_SET) != (off_t)c.offset || read(fd,sm1_packet,n) != (ssize_t)n) return 0;
    return sm1_validate_view(sm1_packet,n,c,h);
}

static unsigned sm1_buffered_frame(struct codec_api *api, struct sm1_cursor c, struct sm1_header *h)
{
    if (c.offset >= route_bytes ||
        (api->curpos != (off_t)c.offset && !api->seek_buffer(c.offset))) return 0;
    if (route_mp4 && c.ordinal>=route_packet_count) return 0;
    unsigned n=route_mp4?route_packets[c.ordinal].size:MIN(route_bytes-c.offset,route_bound+32);
    size_t available;
    const uint8_t *data = api->request_buffer(&available,n);
    if (data && available >= n) {
        unsigned bytes = sm1_validate_view(data,n,c,h);
        if (!bytes) return 0;
        memcpy(sm1_packet,data,bytes);
        api->advance_buffer(bytes);
        return bytes;
    }
    /* Handle a ring-buffer wrap without probing beyond the available bytes. */
    unsigned got = 0;
    while (got < n) {
        size_t amount = api->read_filebuf(sm1_packet+got,n-got);
        if (!amount) return 0;
        got += amount;
    }
    unsigned bytes = sm1_validate_view(sm1_packet,n,c,h);
    return bytes && api->seek_buffer(c.offset+bytes) ? bytes : 0;
}

/* Reuse verified anchors. Cold forward seeks scan only as far as the target,
 * yield per packet and honor stop/replacement seeks every sixteen packets. */
static bool sm1_locate(struct codec_api *api,int fd,unsigned *target,struct sm1_cursor *out,
                       unsigned *trim,bool *halt)
{
    if (route_mp4) {
        unsigned lo=0,hi=route_packet_count;
        if (*target==route_total) { *out=(struct sm1_cursor){route_bytes,route_total,hi}; *trim=0;return true; }
        while (hi-lo>1) { unsigned mid=lo+(hi-lo)/2;
            if (route_packets[mid].sample<=*target) lo=mid; else hi=mid; }
        *out=(struct sm1_cursor){route_packets[lo].offset,route_packets[lo].sample,lo};
        *trim=*target-out->first;return true;
    }
    for (;;) {
        struct sm1_cursor c = sm1_anchors[0];
        for (unsigned i=1;i<anchor_count && sm1_anchors[i].first<=*target;++i) c=sm1_anchors[i];
        while (c.first < *target) {
            if (!(seek_scanned % 16)) {
                intptr_t param = 0;
                long action = api->get_command(&param);
                if (action == CODEC_ACTION_HALT) { *halt=true; return false; }
                if (action == CODEC_ACTION_SEEK_TIME) {
                    *target=MIN((uint64_t)MAX(param,0)*route_rate/1000,route_total); break;
                }
            }
            struct sm1_header h;
            unsigned bytes = sm1_file_frame(fd,c,&h);
            if (!bytes) return false;
            ++seek_scanned;
            sm1_anchor(c);
            if (*target < c.first+h.block) { *out=c; *trim=*target-c.first; return true; }
            c.offset+=bytes; c.first+=h.block; ++c.ordinal;
            api->yield();
        }
        if (c.first==*target) { *out=c; *trim=0; return true; }
    }
}
