#include "sm1_pcm_cache.h"
#define SM1_CACHE_DIR ROCKBOX_DIR "/sm1/pcm-cache"

/* Only the ordinary codec thread owns the reader and the one staging writer. */
static struct spc pcm_cache;
struct spc_memo {int fd; bool active; uint64_t generation; uint8_t digest[32];};
static struct spc_memo source_memo[3],image_memo[2];
static unsigned source_hand,image_hand;
static bool memo_valid(struct spc_memo *m){
    uint64_t now;
    return m && m->active && rbfs_pcm_generation(m->fd,&now) && now==m->generation;
}
struct spc_command {bool halt; long seek_ms; unsigned validated_bytes,blocks;};
static struct spc_memo *spc_identity(struct codec_api *api,const char *path,int actual,
                                     struct spc_memo *slots,unsigned count,unsigned *hand,
                                     struct spc_command *cmd){
    for(unsigned i=0;i<count;i++){
        if(memo_valid(&slots[i])&&fsamefile(actual,slots[i].fd)==1)return &slots[i];
    }
    unsigned ix=(*hand)++%count;struct spc_memo *m=slots+ix;
    if(m->active)close(m->fd);
    m->active=false;m->fd=open(path,O_RDONLY);if(m->fd<0)return NULL;
    if(fsamefile(m->fd,actual)!=1||!rbfs_pcm_generation(m->fd,&m->generation))goto bad;
    struct b2_hash h;b2_init(&h);
    for(;;){
        intptr_t param=0;long act=api->get_command(&param);
        if(act==CODEC_ACTION_HALT||sm1_stop_requested){cmd->halt=true;goto bad;}
        if(act==CODEC_ACTION_SEEK_TIME)cmd->seek_ms=MAX(param,0);
        ssize_t n=read(m->fd,pcm_cache.pcm,sizeof(pcm_cache.pcm));
        if(n<0)goto bad;
        if(!n)break;
        b2_add(&h,pcm_cache.pcm,n);cmd->validated_bytes+=(unsigned)n;api->yield();
    }
    b2_end(&h,m->digest);m->active=true;
    if(memo_valid(m))return m;
bad:
    close(m->fd);m->active=false;return NULL;
}
static bool spc_play(struct codec_api *api,struct spc_memo *src,struct spc_memo *img,
                     unsigned pairs,unsigned initial,struct spc_command *cmd,int log){
    unsigned pos=MIN(initial,pairs);
    sm1_ahead_gate=false;
    for(;;){
        intptr_t param=0;long act=api->get_command(&param);
        if(act==CODEC_ACTION_HALT||sm1_stop_requested)return true;
        if(act==CODEC_ACTION_SEEK_TIME)cmd->seek_ms=MAX(param,0);
        if(cmd->seek_ms>=0){
            pos=MIN((uint64_t)cmd->seek_ms*48,pairs);
            if(log>=0)fdprintf(log,"cache seek_ms=%ld output_pair=%u block=%u prior_blocks=%u\n",cmd->seek_ms,pos,pos/SPC_PAIRS,cmd->blocks);
            cmd->seek_ms=-1;api->set_elapsed((uint64_t)pos*1000/48000);api->seek_complete();
        }
        if(pos==pairs){sm1_ahead_release_tail();return true;}
        const int16_t *data;
        if(!memo_valid(src)||!memo_valid(img))return false;
        unsigned n=spc_read(&pcm_cache,pos,&data);
        if(!n){spc_close(&pcm_cache);remove(pcm_cache.final);return false;}
        if(!cmd->blocks && log>=0)fdprintf(log,"cache first_pcm_tick=%ld output_pair=%u block=%u\n",current_tick,pos,pos/SPC_PAIRS);
        ++cmd->blocks;
        api->pcmbuf_insert(data,NULL,n);pos+=n;
        api->set_elapsed((uint64_t)pos*1000/48000);
        /* Cached seeks use sample time; a cache byte offset is never a compressed bookmark. */
        api->set_offset(0);api->yield();
    }
}
