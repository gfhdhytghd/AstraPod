extern volatile bool sm1_ahead_gate;
extern bool sm1_ahead_fits;
#include "sm1_flac_profile.h"
static const struct sm1_flac_profile *route_profile;
static bool conversion_index;
static struct mutex sm1_owner;
extern unsigned sm1_ahead_target;
#include "dsp_proc_entry.h"
#define SM1_ROUTE44 SM1_TEST_DIR "/codec44.flac"
#define SM1_ROUTE48 SM1_TEST_DIR "/codec48.flac"
static unsigned route_depth, route_rate, route_total, route_serial;
static volatile unsigned route_hw_frames, route_seeks, route_stock_runs;
#define SM4_PACKET_TABLE
#include "../../../../lib/rbcodec/metadata/sm1_fmp4.h"
static struct sm4_packet route_packets[8192];
static uint32_t route_packet_count;
static bool route_mp4;
#include "sm1_stream_index.inc"

static const char *sm1_stream_config(const uint8_t *b,unsigned *bound,unsigned *maxblock)
{
    *maxblock = (b[2] << 8) | b[3];
    *bound = b[7] << 16 | b[8] << 8 | b[9];
    route_rate = b[10] << 12 | b[11] << 4 | b[12] >> 4;
    unsigned channels = 1 + ((b[12] >> 1) & 7);
    route_depth = 1 + (((b[12] & 1) << 4) | b[13] >> 4);
    route_total = (uint32_t)b[14] << 24 | b[15] << 16 | b[16] << 8 | b[17];
    if (route_depth != 16 && route_depth != 24) return "sample-depth";
    if (channels != 2) return "channel-count";
    route_profile = sm1_profile_for(route_rate,route_depth);
    if (!route_profile) return "sample-rate-or-depth";
    if (route_profile->decimation > 1 && !conversion_index && (!sm1_ahead_target || !sm1_ahead_fits)) return "ahead-buffer-required";
    if (!*maxblock || *maxblock > route_profile->max_block) return "block-limit";
    if (*bound > route_profile->max_packet || *bound > SM1_PACKET_MAX) return "packet-limit";
    if ((b[13] & 15) || !route_total) return "unsupported-total-samples";
    if (!*bound) *bound = MIN(route_profile->max_packet,SM1_PACKET_MAX);
    return NULL;
}

/* Bound RAM and startup work. Use a separate read-only handle for lookahead,
 * leaving the codec thread's normal buffered input position untouched. */
static const char *sm1_index_file(int fd, uint32_t length, uint32_t start_us)
{
    uint8_t b[34];
    route_mp4=false; route_packet_count=0;
    route_bytes = length;
    startup_bytes = 4;
    if (read(fd,b,4)!=4) return "source-read";
    if (memcmp(b,"fLaC",4)) {
        sm4_index_bytes=0;
        struct sm4_info info;
        if (!sm4_identify(fd,length,&info)) return "unsupported-container";
        const char *reason=sm1_stream_config(info.stream,&route_bound,&route_maxblock);
        if (reason) return reason;
        if (!sm4_index(fd,length,&info,route_packets,8192,&route_packet_count))
            return "fragment-table";
        route_mp4=true;
        anchor_count=0; anchor_stride=32; seek_scanned=0;
        struct sm1_cursor first={.offset=route_packets[0].offset};
        struct sm1_header h;
        if (!sm1_file_frame(fd,first,&h)) return "container-first-frame";
        sm1_anchor(first);
        startup_bytes=4+sm4_index_bytes+route_packets[0].size;
        return NULL;
    }
    unsigned pos = 4, bound = 0, maxblock = 0;
    for (unsigned m = 0;; ++m) {
        if (m == 128 || pos > length || length - pos < 4 || read(fd, b, 4) != 4)
            return "metadata-or-io";
        startup_bytes += 4;
        unsigned type = b[0] & 127;
        bool last = b[0] & 128;
        unsigned size = b[1] << 16 | b[2] << 8 | b[3];
        pos += 4;
        if (size > length - pos || type == 127) return "metadata-or-io";
        if (!m) {
            if (type || size != sizeof(b) || read(fd,b,sizeof(b)) != sizeof(b)) return "metadata-or-io";
            startup_bytes += sizeof(b);
            const char *reason=sm1_stream_config(b,&bound,&maxblock);
            if (reason) return reason;
        } else if (!type || lseek(fd, pos + size, SEEK_SET) != (off_t)(pos + size))
            return "metadata-or-io";
        pos += size;
        if (last) break;
    }
    route_bound=bound;
    route_maxblock=maxblock;
    anchor_count=0; anchor_stride=32; seek_scanned=0;
    struct sm1_cursor first={.offset=pos};
    struct sm1_header h;
    if ((uint32_t)(USEC_TIMER-start_us) >= 2000000u) return "metadata-time-limit";
    if (!sm1_file_frame(fd,first,&h)) return "first-frame-crc-or-boundary";
    startup_bytes += MIN(route_bytes-pos,route_bound+32);
    sm1_anchor(first);
    return NULL;
}

extern volatile bool sm1_monitor_pcm;
extern volatile unsigned sm1_pcm_starts, sm1_pcm_empty, sm1_pcm_initial, sm1_pcm_low, sm1_pcm_peak;
struct sm1_frame_report { uint32_t ordinal, trim, compressed_crc, output_crc, frame_us; };
#ifdef ASTRA_SM1_TEST
static struct sm1_frame_report sm1_reports[8192];
#endif
#include "sm1_ahead96.inc"
static int sm1_play_owned(struct codec_api *api)
{
    uint32_t begin = USEC_TIMER;
    unsigned resume_ms = api->id3->elapsed;
    unsigned resume_offset = api->id3->offset;
    int source = open(api->id3->path, O_RDONLY);
    int image = -1;
    int log = -1;
#ifdef ASTRA_SM1_TEST
    log=open(SM1_TEST_DIR "/codec-trace.txt",O_WRONLY|O_CREAT|O_APPEND,0666);
#endif
    const char *reason = "index-error";
    const unsigned serial = ++route_serial;
    unsigned crc = 0xffffffffu, emitted = 0, first = 0, skip = 0, submitted = 0;
    unsigned index_us = 0, peak_us=0, over_budget=0;
    bool valid = false;
    const char *reject = "source-unavailable";
    if (source >= 0 && api->filesize >= 0 && (uint64_t)api->filesize <= UINT32_MAX)
        reject = sm1_index_file(source, (uint32_t)api->filesize, begin);
    if (reject) {
        if (sm1_ahead_target) sm1_ahead_gate=false;
        ++route_stock_runs;
        if (log >= 0) {
            fdprintf(log,"decline run=%u backend=stock reason=%s index_us=%lu source=%s\n",
                     serial,reject,(unsigned long)(USEC_TIMER-begin),api->id3->path);
            close(log);
        }
        if (source >= 0) close(source);
        return 1;
    }
    index_us = USEC_TIMER - begin;
    if (api->id3->frequency != route_rate) goto finish;
    if (route_profile->decimation > 1)
        return sm1_ahead_run(api,source,log,route_profile);
    char image_path[MAX_PATH];
    snprintf(image_path,sizeof(image_path),"%s/%s",SM1_TEST_DIR,route_profile->image);
    image = open(image_path,O_RDONLY);
    reason = "image-error";
    if (image < 0 || !sm1_test_image(image,-1) || !sm1_begin(image)) goto finish;
    api->configure(DSP_SET_FREQUENCY,route_rate);
    api->configure(DSP_SET_SAMPLE_DEPTH,16);
    api->configure(DSP_SET_STEREO_MODE,STEREO_INTERLEAVED);
    struct dsp_replay_gains gains = { .track_gain=api->id3->track_gain, .album_gain=api->id3->album_gain,
        .track_peak=api->id3->track_peak, .album_peak=api->id3->album_peak };
    api->configure(REPLAYGAIN_SET_GAINS,(intptr_t)&gains);
    struct sm1_cursor c=sm1_anchors[0];
    bool halted=false;
    if (resume_ms) {
        first=MIN((uint64_t)resume_ms*route_rate/1000,route_total);
        if (!sm1_locate(api,source,&first,&c,&skip,&halted)) { reason=halted ? "halt":"resume-error"; valid=halted;goto finish; }
    } else if (resume_offset>c.offset) {
        /* Resolve an offset resume to its containing verified frame. */
        while (c.first<route_total) {
            if (route_mp4) c.offset=route_packets[c.ordinal].offset;
            struct sm1_header h;
            unsigned bytes=sm1_file_frame(source,c,&h);
            if (!bytes) { reason="resume-frame-error"; goto finish; }
            if (resume_offset<c.offset+bytes) break;
            sm1_anchor(c); c.offset+=bytes; c.first+=h.block; ++c.ordinal; api->yield();
        }
        first=c.first;
    }
    if (log >= 0) fdprintf(log,"begin run=%u backend=SM1 rate=%u depth=%u container=%s table_packets=%u startup_frames=1 startup_bytes=%u index_us=%u warm=%d initial_sample=%u source=%s\n",
                           serial,route_rate,route_depth,route_mp4?"fMP4":"native",(unsigned)route_packet_count,startup_bytes,index_us,hw.warm,first,api->id3->path);
    sm1_pcm_starts=sm1_pcm_empty=sm1_pcm_initial=sm1_pcm_peak=0;
    sm1_pcm_low=UINT_MAX;
    sm1_monitor_pcm=true;
    for (;;) {
        intptr_t param = 0;
        long action = api->get_command(&param);
        if (action == CODEC_ACTION_HALT) { reason = "halt"; valid = true; break; }
        if (action == CODEC_ACTION_SEEK_TIME) {
            sm1_monitor_pcm=false;
            if (log >= 0)
                fdprintf(log,"prefix run=%u segment_first=%u segment_count=%u crc=%08x\n",serial,first,emitted,crc);
            first = MIN((uint64_t)MAX(param,0) * route_rate / 1000, route_total);
            unsigned seek_start=USEC_TIMER, old_scanned=seek_scanned;
            reason="seek-error";
            if (!sm1_locate(api,source,&first,&c,&skip,&halted)) {
                if (halted) { valid=true;reason="halt"; } break;
            }
            if (!api->seek_buffer(c.offset)) break;
            crc=0xffffffffu;emitted=0;++route_seeks;
            if (log>=0) fdprintf(log,"seek run=%u ms=%ld sample=%u frame=%u trim=%u scanned=%u seek_us=%lu\n",
                serial,(long)param,first,c.ordinal,skip,seek_scanned-old_scanned,(unsigned long)(USEC_TIMER-seek_start));
            api->set_elapsed((uint64_t)first*1000/route_rate);
            api->seek_complete();
            sm1_monitor_pcm=true;
        }
        if (route_mp4) c.offset=c.ordinal<route_packet_count?route_packets[c.ordinal].offset:route_bytes;
        if (c.first == route_total) {
            valid=c.offset==route_bytes; reason=valid ? "eof":"trailing-data"; break;
        }
        reason="frame-crc-or-boundary";
        struct sm1_header h;
        unsigned bytes=sm1_buffered_frame(api,c,&h);
        if (!bytes) break;
        sm1_anchor(c);
        unsigned samples=h.block;
        reason = "dsp-error";
#ifdef PODBOX_SM1_DMA_PROBE
        probe_playback_frame = route_depth == 16 && !global_settings.eq_enabled
            && (route_rate == 44100 || route_rate == 48000);
#endif
        bool decoded = sm1_packet_decode(bytes,2*samples);
#ifdef PODBOX_SM1_DMA_PROBE
        probe_playback_frame = false;
#endif
        if (!decoded) break;
#ifdef ASTRA_SM1_TEST
        if (submitted<ARRAYLEN(sm1_reports)) {
            sm1_reports[submitted]=(struct sm1_frame_report){c.ordinal,skip,
                crc_32(sm1_packet,bytes,0xffffffffu),crc_32(sm1_output,4*samples,0xffffffffu),sm1_last_us};
        }
#endif
        peak_us=MAX(peak_us,sm1_last_us);
        if ((uint64_t)sm1_last_us*route_rate > (uint64_t)samples*1000000) ++over_budget;
        ++submitted; ++route_hw_frames;
        crc = crc_32(sm1_output + 2*skip,4*(samples-skip),crc);
        emitted += samples - skip;
        api->pcmbuf_insert(sm1_output + 2*skip,NULL,samples-skip);
        unsigned end = c.first + samples;
        api->set_elapsed((uint64_t)end*1000/route_rate);
        api->set_offset(api->curpos);
        skip=0; c.offset+=bytes; c.first=end; ++c.ordinal;
    }
finish:
    if (valid && !strcmp(reason,"eof")) sm1_ahead_release_tail();
    sm1_monitor_pcm=false;
    #ifdef ASTRA_SM1_TEST
    int report=open(SM1_TEST_DIR "/codec-frames.bin",O_WRONLY|O_CREAT|O_TRUNC,0666);
    if (report>=0) {
        write(report,sm1_reports,MIN(submitted,ARRAYLEN(sm1_reports))*sizeof(sm1_reports[0]));
        close(report);
    }
    #endif
    if (log >= 0) {
        fdprintf(log,"pcm starts=%u empty_while_decoding=%u initial_bytes=%u low_bytes=%u peak_bytes=%u\n",
                 sm1_pcm_starts,sm1_pcm_empty,sm1_pcm_initial,sm1_pcm_low,sm1_pcm_peak);
        fdprintf(log,"end run=%u backend=SM1 reason=%s sm1_frames=%u segment_first=%u segment_count=%u crc=%08x anchors=%u stride=%u peak_us=%u over_frame_budget=%u fault=%s\n",
                 serial,reason,submitted,first,emitted,crc,anchor_count,anchor_stride,peak_us,over_budget,hw.owned && hw.fault ? hw.fault : "-");
        close(log);
    }
    sm1_end();
    if (image >= 0) close(image);
    if (source >= 0) close(source);
    return valid ? CODEC_OK : CODEC_ERROR;
}

int sm1_route_run(struct codec_api *api)
{
    mutex_lock(&sm1_owner);
    int result=sm1_play_owned(api);
    mutex_unlock(&sm1_owner);
    return result;
}
#include "sm1_convert.inc"
#include "sm1_alac.inc"
