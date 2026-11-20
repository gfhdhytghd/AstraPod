#include "audio/sm1_convert.h"
#include "audio/track_decode.h"
#include "flac_output.inc"
#include "convert_resample.inc"

/* Only the settings conversion task writes derivatives. Playback lookup
 * uses a separate source descriptor and local hash context. */
static struct flac_output conv_out;
static struct mp3entry conv_id3;
static struct mutex convert_owner;
static bool (*conv_cancel)(void);
static bool conv_failed;
static uint8_t conv_window[65536] __attribute__((aligned(32)));

void sm1_convert_init(void)
{
    mutex_init(&convert_owner);
    mutex_init(&sm1_owner);
}

static bool conv_name(const char *src,char *dest,size_t cap)
{
    int n=snprintf(dest,cap,"%s.48k.flac",src);
    return n>0 && (size_t)n<cap;
}

static bool conv_hash(int fd,uint8_t digest[32],bool (*cancel)(void))
{
    uint8_t buf[512];
    struct b2_hash h;
    if(lseek(fd,0,SEEK_SET)!=0)return false;
    b2_init(&h);
    unsigned count=0;
    for (;;) {
        ssize_t n=read(fd,buf,sizeof buf);
        if(n<0)return false;
        if(!n)break;
        b2_add(&h,buf,n);
        if((++count&63)==0){if(cancel && cancel())return false;yield();}
    }
    b2_end(&h,digest);return true;
}

/* APPLICATION payload contains the writer version, source BLAKE2s and
 * final file length. The stream also has standard PCM MD5 and frame CRCs. */
static bool conv_marker(int fd,const uint8_t digest[32],uint64_t len)
{
    uint8_t app[52]={0x02,0,0,48,'P','B','4','8',1,0,0,0};
    memcpy(app+12,digest,32);
    for(int i=0;i<8;++i){app[44+i]=len;len>>=8;}
    return lseek(fd,42,SEEK_SET)==42 && flac_put(fd,app,sizeof app);
}

/* Keep the ordinary metadata fields that Rockbox exposes to the browser. */
static bool conv_le32(int fd,uint32_t v)
{
    uint8_t b[4]={v,v>>8,v>>16,v>>24};
    return flac_put(fd,b,4);
}

static bool conv_comments(int fd)
{
    const char *keys[]={"TITLE","ARTIST","ALBUM","ALBUMARTIST","GENRE","DATE",
                        "TRACKNUMBER","DISCNUMBER","COMPOSER","COMMENT","GROUPING"};
    const char *values[]={conv_id3.title,conv_id3.artist,conv_id3.album,conv_id3.albumartist,
        conv_id3.genre_string,conv_id3.year_string,conv_id3.track_string,conv_id3.disc_string,
        conv_id3.composer,conv_id3.comment,conv_id3.grouping};
    unsigned count=0,size=8+6;
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i)
        if(values[i] && values[i][0]){size+=5+strlen(keys[i])+strlen(values[i]);++count;}
    uint8_t h[4]={0x84,size>>16,size>>8,size};
    if(!flac_put(fd,h,4)||!conv_le32(fd,6)||!flac_put(fd,"PodBox",6)||!conv_le32(fd,count))return false;
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i){
        if(!values[i]||!values[i][0])continue;
        size_t k=strlen(keys[i]),v=strlen(values[i]);
        if(!conv_le32(fd,k+1+v)||!flac_put(fd,keys[i],k)||!flac_put(fd,"=",1)||
           !flac_put(fd,values[i],v))return false;
    }
    return true;
}

static bool conv_matches(int fd,const char *dest,const uint8_t digest[32])
{
    uint8_t h[94];int out=open(dest,O_RDONLY);
    if(out<0)return false;
    bool ok=read(out,h,sizeof h)==sizeof h && !memcmp(h,"fLaC",4) &&
        h[4]==0 && h[7]==34 && h[42]==0x02 && h[45]==48 &&
        !memcmp(h+46,"PB48\1\0\0\0",8) && !memcmp(h+54,digest,32) &&
        fsamefile(fd,out)==0;
    uint64_t size=0;
    if(ok){for(int i=7;i>=0;--i)size=(size<<8)|h[86+i];ok=size>94 && size==(uint64_t)ffilesize(out);}
    close(out);return ok;
}

bool sm1_converted_path(const char *source,char *dest,size_t capacity)
{
    if(!conv_name(source,dest,capacity)||!file_exists(dest))return false;
    int fd=open(source,O_RDONLY);if(fd<0)return false;
    uint8_t digest[32];
    bool ok=conv_hash(fd,digest,NULL)&&conv_matches(fd,dest,digest);
    close(fd);return ok;
}

static bool conv_aborted(void){return conv_cancel && conv_cancel();}

static int conv_sm1(int fd)
{
    int result=SM1_CONVERT_ERROR;
    off_t size=ffilesize(fd);
    if(size<=0 || (uint64_t)size>UINT32_MAX || lseek(fd,0,SEEK_SET)!=0)return result;
    conversion_index=true;
    const char *reason=sm1_index_file(fd,(uint32_t)size,USEC_TIMER);
    conversion_index=false;
    if(reason || route_profile->decimation<2)return SM1_CONVERT_UNSUPPORTED;
    char name[MAX_PATH];snprintf(name,sizeof name,"%s/%s",SM1_TEST_DIR,route_profile->image);
    int image=open(name,O_RDONLY);
    if(image<0)return SM1_CONVERT_UNSUPPORTED;
    unsigned factor=route_profile->decimation;
    struct sm1_cursor c=sm1_anchors[0];
    if(!sm1_test_image(image,-1)||!sm1_begin(image))goto finish;
    while(c.first<route_total) {
        if(conv_aborted()){result=SM1_CONVERT_CANCELLED;goto finish;}
        struct sm1_header h;
        unsigned bytes=sm1_file_frame(fd,c,&h);
        if(!bytes || c.first%factor || (h.block%factor && h.block!=route_total-c.first))goto finish;
        unsigned n=sm1_output_pairs(h.block,factor);
        if(!sm1_packet_decode(bytes,n*2)||!flac_out_add(&conv_out,sm1_output,n))goto finish;
        c.first+=h.block;++c.ordinal;
        c.offset=route_mp4 && c.ordinal<route_packet_count?route_packets[c.ordinal].offset:c.offset+bytes;
        yield();
    }
    if(c.first==route_total && (route_mp4?c.ordinal==route_packet_count:c.offset==route_bytes))
        result=SM1_CONVERT_OK;
finish:
    sm1_end();close(image);return result;
}

static int32_t conv_sample(const void *buf,unsigned i,const struct track_pcm *fmt)
{
    if(fmt->packed16)return ((const int16_t*)buf)[i]*256;
    int64_t v=((const int32_t*)buf)[i];
    if(fmt->shift>=8)v>>=fmt->shift-8;else v*=1u<<(8-fmt->shift);
    return v< -8388608?-8388608:v>8388607?8388607:v;
}

static void conv_sink(const void *left,const void *right,int count,const struct track_pcm *fmt,
                      unsigned long time_ms)
{
    (void)time_ms;
    if(conv_failed)return;
    if(fmt->frequency!=rs.rate){conv_failed=true;return;}
    for(int i=0;i<count;++i){
        unsigned n=fmt->stereo_mode==STEREO_INTERLEAVED?2*i:i;
        int32_t a=conv_sample(left,n,fmt);
        int32_t b=fmt->stereo_mode==STEREO_MONO?a:
                  fmt->stereo_mode==STEREO_INTERLEAVED?conv_sample(left,n+1,fmt):
                  right?conv_sample(right,n,fmt):a;
        rs.ring[rs.input&1023][0]=a;rs.ring[rs.input&1023][1]=b;++rs.input;
        if(rs.center+rs.radius<rs.input && !rs_emit(&conv_out)){conv_failed=true;break;}
    }
}

static bool conv_stop_arm(void){return conv_failed || conv_aborted();}

static int conv_arm(const char *path)
{
    if(!rs_init(conv_id3.frequency))return SM1_CONVERT_UNSUPPORTED;
    conv_failed=false;
    int rc=track_decode_run(path,0,~0ul,conv_window,sizeof conv_window,conv_sink,NULL,conv_stop_arm,NULL);
    if(conv_failed)return SM1_CONVERT_ERROR;
    if(rc==TRACK_DECODE_ABORTED)return SM1_CONVERT_CANCELLED;
    if(rc==TRACK_DECODE_NO_CODEC)return SM1_CONVERT_UNSUPPORTED;
    if(rc!=TRACK_DECODE_OK || !rs.input)return SM1_CONVERT_ERROR;
    if(conv_id3.samples && conv_id3.samples!=rs.input)return SM1_CONVERT_ERROR;
    while(rs.center<rs.input)if(!rs_emit(&conv_out))return SM1_CONVERT_ERROR;
    conv_out.expected=rs.emitted;
    return SM1_CONVERT_OK;
}

int sm1_convert_track(const char *source,unsigned rate,bool (*cancelled)(void))
{
    if(rate!=48000)return SM1_CONVERT_UNSUPPORTED;
    char dest[MAX_PATH],part[MAX_PATH];
    if(!conv_name(source,dest,sizeof dest))return SM1_CONVERT_ERROR;
    int fd=open(source,O_RDONLY);if(fd<0)return SM1_CONVERT_ERROR;
    int result=SM1_CONVERT_ERROR,out=-1;
    bool own_part=false;
    mutex_lock(&convert_owner);conv_cancel=cancelled;
    uint8_t digest[32];uint64_t gen,now;
    if(!rbfs_pcm_generation(fd,&gen)||!conv_hash(fd,digest,cancelled)){
        if(conv_aborted())result=SM1_CONVERT_CANCELLED;
        goto end;
    }
    if(file_exists(dest)){result=conv_matches(fd,dest,digest)?SM1_CONVERT_EXISTS:SM1_CONVERT_ERROR;goto end;}
    memset(&conv_id3,0,sizeof conv_id3);
    if(!get_metadata(&conv_id3,fd,source)||conv_id3.frequency<=48000){result=SM1_CONVERT_UNSUPPORTED;goto end;}
    /* Exclusively create a fresh same-directory temporary. A stale file
     * from a power cut cannot clobber a user file or prevent retry. */
    for(unsigned i=0;i<100 && out<0;++i){
        int len=snprintf(part,sizeof part,"%s.tmp%u",dest,i);
        if(len<0 || (unsigned)len>=sizeof part)goto end;
        out=open(part,O_CREAT|O_EXCL|O_RDWR,0666);
        if(out<0 && !file_exists(part))goto end;
    }
    if(out<0)goto end;
    own_part=true;
    uint64_t expected=conv_id3.samples?
        (conv_id3.samples*48000+conv_id3.frequency-1)/conv_id3.frequency:0xffffffffu;
    if(!flac_out_open(&conv_out,out,expected)||!conv_marker(out,digest,0)||!conv_comments(out))goto end;
    result=SM1_CONVERT_UNSUPPORTED;
    if(conv_id3.codectype==AFMT_FLAC && (conv_id3.frequency==96000 || conv_id3.frequency==192000)){
        mutex_lock(&sm1_owner);
        result=conv_sm1(fd);
        if(result==SM1_CONVERT_OK)conv_out.expected=sm1_output_pairs(route_total,route_profile->decimation);
        mutex_unlock(&sm1_owner);
    }
    if(result==SM1_CONVERT_UNSUPPORTED)result=conv_arm(source);
    if(result!=SM1_CONVERT_OK)goto end;
    result=SM1_CONVERT_ERROR;
    if(conv_aborted()){result=SM1_CONVERT_CANCELLED;goto end;}
    if(!rbfs_pcm_generation(fd,&now)||now!=gen || fsamefile(fd,out)!=0 ||
       !flac_out_finish(&conv_out)||!conv_marker(out,digest,ffilesize(out))||fsync(out)<0)goto end;
    int current=open(source,O_RDONLY);
    if(current<0)goto end;
    bool same=fsamefile(fd,current)==1 && rbfs_pcm_generation(current,&now) && now==gen;
    close(current);
    if(!same)goto end;
    if(conv_aborted()){result=SM1_CONVERT_CANCELLED;goto end;}
    int cr=close(out);out=-1;
    if(cr<0 || file_exists(dest) || rename(part,dest)<0)goto end;
    own_part=false;result=SM1_CONVERT_OK;
end:
    if(out>=0)close(out);
    if(own_part)remove(part);
    close(fd);conv_cancel=NULL;mutex_unlock(&convert_owner);return result;
}
