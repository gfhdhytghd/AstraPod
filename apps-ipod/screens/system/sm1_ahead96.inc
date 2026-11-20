#include "pcm_mixer.h"
extern void sm1_ahead_release_tail(void);
/* Explicit test fixture. The ordinary PCM driver consumes each output frame.
 * Predictive and filter history are rebuilt from frame zero for a seek. */
extern unsigned sm1_ahead_target, sm1_ahead_pool, sm1_ahead_layout, sm1_ahead_file_min;
extern bool sm1_ahead_fits;
extern volatile bool sm1_ahead_gate;
extern volatile long sm1_ahead_armed_tick, sm1_ahead_started_tick;
extern volatile unsigned sm1_ahead_initial_bytes;
#include "sm1_cache_route.inc"
static int sm1_ahead_run(struct codec_api *api,int fd,int log,const struct sm1_flac_profile *profile)
{
    unsigned factor=profile->decimation;
    char image_path[MAX_PATH];
    snprintf(image_path,sizeof(image_path),"%s/%s",SM1_TEST_DIR,profile->image);
    int image=open(image_path,O_RDONLY);
    const char *reason="admission";
    unsigned hw_frames=0,emitted=0,crc=0xffffffffu,from=0,peak=0,slow=0,trim=0;
    unsigned output_base=0,discarded=0;
    unsigned resume_offset=api->id3->elapsed ? 0 : api->id3->offset;
    bool good=false,writing=false;
    struct spc_memo *source_id=NULL,*image_id=NULL;
    struct spc_command cache_cmd={.seek_ms=-1};
    unsigned cache_pairs=sm1_output_pairs(route_total,factor);
    struct sm1_cursor c={0};
    if (!sm1_ahead_fits || fd<0 || image<0 ||
        api->filesize<=0 || (uint64_t)api->filesize>UINT32_MAX ||
        route_mp4 ||
        mixer_get_frequency()!=48000) goto end;

    api->configure(DSP_SET_FREQUENCY,48000);
    api->configure(DSP_SET_SAMPLE_DEPTH,16);
    api->configure(DSP_SET_STEREO_MODE,STEREO_INTERLEAVED);
    struct dsp_replay_gains gains={.track_gain=api->id3->track_gain,.album_gain=api->id3->album_gain,
        .track_peak=api->id3->track_peak,.album_peak=api->id3->album_peak};
    api->configure(REPLAYGAIN_SET_GAINS,(intptr_t)&gains);
    from=MIN((uint64_t)api->id3->elapsed*route_rate/1000,route_total);
    output_base=sm1_output_pairs(from,factor);
    source_id=spc_identity(api,api->id3->path,fd,source_memo,3,&source_hand,&cache_cmd);
    if(!cache_cmd.halt && source_id)
        image_id=spc_identity(api,image_path,image,image_memo,2,&image_hand,&cache_cmd);
    if(log>=0)fdprintf(log,"cache identity_tick=%ld validated_bytes=%u rate=%u source_samples=%u output_pairs=%u\n",current_tick,cache_cmd.validated_bytes,route_rate,route_total,cache_pairs);
    if(cache_cmd.halt){reason="halt";good=true;goto end;}
    if(source_id && image_id && spc_init(&pcm_cache,SM1_CACHE_DIR,source_id->digest,
                                         image_id->digest,route_rate,cache_pairs)){
        if(spc_open(&pcm_cache)){
            /* An offset-only bookmark needs the original FLAC frame map. The
               continuous decoder resolves it as usual without guessing a PCM offset. */
            if(!resume_offset || api->id3->elapsed || cache_cmd.seek_ms>=0){
                sm1_pcm_starts=sm1_pcm_empty=sm1_pcm_initial=sm1_pcm_peak=0;
                sm1_pcm_low=UINT_MAX;sm1_monitor_pcm=true;
                if(log>=0)fdprintf(log,"cache hit tick=%ld\n",current_tick);
                good=spc_play(api,source_id,image_id,cache_pairs,output_base,&cache_cmd,log);
                if(log>=0)fdprintf(log,"cache returned blocks=%u sm1_submitted=0 tick=%ld\n",cache_cmd.blocks,current_tick);
                reason=good?"cache-finished":"cache-data";spc_close(&pcm_cache);goto end;
            }
            spc_close(&pcm_cache);
        }
        mkdir(SM1_CACHE_DIR);
        writing=spc_begin(&pcm_cache);
        if(log>=0)fdprintf(log,"cache miss writer=%d tick=%ld\n",writing,current_tick);
    }
    if(cache_cmd.seek_ms>=0){
        from=MIN((uint64_t)cache_cmd.seek_ms*route_rate/1000,route_total);
        output_base=sm1_output_pairs(from,factor);resume_offset=0;
        api->set_elapsed((uint64_t)from*1000/route_rate);api->seek_complete();
    }
    if (!sm1_test_image(image,-1) || !sm1_begin(image)) {reason="image";goto end;}
    c=sm1_anchors[0];
    sm1_pcm_starts=sm1_pcm_empty=sm1_pcm_initial=sm1_pcm_peak=0;
    sm1_pcm_low=UINT_MAX; sm1_monitor_pcm=true;
    if (log>=0) fdprintf(log,"begin backend=SM1 container=native rate=%u output=48000 ahead=1 target_bytes=%u pool_bytes=%u pcm_layout_bytes=%u compressed_min=%u\n",
             route_rate,sm1_ahead_target,sm1_ahead_pool,sm1_ahead_layout,sm1_ahead_file_min);
    for (;;) {
        intptr_t param=0;
        long action=api->get_command(&param);
        if (action==CODEC_ACTION_HALT || sm1_stop_requested) {reason="halt";good=true;break;}
        if (action==CODEC_ACTION_SEEK_TIME) {
            if(writing){spc_abort(&pcm_cache);writing=false;}
            if (log>=0) fdprintf(log,"prefix segment_first=%u segment_count=%u crc=%08x\n",output_base,emitted,crc);
            sm1_ahead_gate=true;sm1_ahead_armed_tick=current_tick;sm1_ahead_started_tick=-1;
            sm1_ahead_initial_bytes=0;
            resume_offset=0;
            from=MIN((uint64_t)MAX(param,0)*route_rate/1000,route_total);
            output_base=sm1_output_pairs(from,factor);emitted=0;crc=0xffffffffu;discarded=0;
            c=sm1_anchors[0];
            sm1_end();
            if (!sm1_begin(image) || !api->seek_buffer(c.offset)) {reason="seek-reset";break;}
            ++route_seeks;
            api->set_elapsed((uint64_t)from*1000/route_rate);
            api->seek_complete();
            if (log>=0) fdprintf(log,"seek ms=%ld source_sample=%u output_sample=%u replay_from=0\n",
                     (long)param,from,output_base);
        }
        if (c.first==route_total) {
            good=c.offset==route_bytes;reason=good?"eof":"trailing-data";break;
        }
        struct sm1_header h;
        unsigned bytes=sm1_buffered_frame(api,c,&h);
        if (!bytes || (c.first%factor) || ((h.block%factor) && h.block!=route_total-c.first)) {
            reason="frame-boundary-or-phase";break;
        }
        unsigned count=sm1_output_pairs(h.block,factor);
        if (!sm1_packet_decode(bytes,2*count)) {reason="dsp-error";break;}
        if(writing && !spc_append(&pcm_cache,sm1_output,count)){spc_abort(&pcm_cache);writing=false;}
        if(!hw_frames && log>=0)fdprintf(log,"cache sm1_first_tick=%ld\n",current_tick);
        ++hw_frames; ++route_hw_frames; peak=MAX(peak,sm1_last_us);
        if ((uint64_t)sm1_last_us*route_rate>(uint64_t)h.block*1000000) ++slow;
        /* Replay from zero to restore all filter sections. An offset-only
           bookmark resumes at the start of its containing verified frame. */
        if (resume_offset) {
            if (resume_offset >= c.offset+bytes)
                from=c.first+h.block;
            else {
                from=c.first;
                resume_offset=0;
            }
            output_base=sm1_output_pairs(from,factor);
        }
        trim=from>c.first ? MIN(count,sm1_output_pairs(from-c.first,factor)):0;
        discarded+=trim;
        if (trim<count) {
            unsigned n=count-trim;
            crc=crc_32(sm1_output+2*trim,4*n,crc);
            api->pcmbuf_insert(sm1_output+2*trim,NULL,n);
            emitted+=n;
            api->set_elapsed((uint64_t)(c.first+h.block)*1000/route_rate);
            api->set_offset(api->curpos);
        }
        ++c.ordinal;c.offset+=bytes;c.first+=h.block;
        api->yield();
    }
end:
    if(writing){
        if(good && !strcmp(reason,"eof") && memo_valid(source_id) && memo_valid(image_id))
        {
            bool published=spc_publish(&pcm_cache);
            if(log>=0)fdprintf(log,"cache published=%d pcm_bytes=%lu sm1_frames=%u last_tick=%ld\n",published,(unsigned long)cache_pairs*4,hw_frames,current_tick);
        }
        else spc_abort(&pcm_cache);
    }
    if (good && !strcmp(reason,"eof")) sm1_ahead_release_tail();
    sm1_monitor_pcm=false;
    sm1_ahead_gate=false;
    if (log>=0) {
        if (log>=0) fdprintf(log,"end backend=SM1 reason=%s sm1_frames=%u segment_first=%u segment_count=%u crc=%08x prefill_ticks=%ld initial_bytes=%u starts=%u empty=%u low=%u peak_bytes=%u peak_us=%u over_frame_budget=%u discarded=%u fits=%d pool_bytes=%u actual_sink_hz=%u fault=%s\n",
            reason,hw_frames,output_base,emitted,crc,
            sm1_ahead_started_tick<0?-1L:sm1_ahead_started_tick-sm1_ahead_armed_tick,sm1_ahead_initial_bytes,
            sm1_pcm_starts,sm1_pcm_empty,sm1_pcm_low,sm1_pcm_peak,peak,slow,discarded,sm1_ahead_fits,sm1_ahead_pool,mixer_get_frequency(),
            hw.owned && hw.fault?hw.fault:"-");
        close(log);
    }
    sm1_end();if (fd>=0) close(fd);if (image>=0) close(image);
    return good?CODEC_OK:CODEC_ERROR;
}
