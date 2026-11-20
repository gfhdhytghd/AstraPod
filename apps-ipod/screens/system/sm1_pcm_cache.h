/* One writer in the codec thread. Readers only open completed .pcm files. */
#ifndef SM1_PCM_CACHE_H
#define SM1_PCM_CACHE_H
#include "blake2s.h"
#define SPC_PAIRS 4096u
#define SPC_HDR 48u
struct spc {
    int fd;
    uint32_t pairs,position;
    unsigned used,block;
    uint8_t key[32];
    int16_t pcm[SPC_PAIRS*2];
    char tmp[256],final[256];
};
static bool spc_io(int fd,void *buf,size_t n,bool writing){
    uint8_t *p=buf;
    while(n){
        ssize_t done=writing?write(fd,p,n):read(fd,p,n);
        if(done<=0)return false;
        p+=done;n-=done;
    }
    return true;
}
#ifdef SPC_HOST_VERIFY
static bool spc_hash_file(int fd,uint8_t result[32],void *scratch,unsigned capacity){
    struct b2_hash hash;b2_init(&hash);
    if(lseek(fd,0,SEEK_SET)!=0)return false;
    for(;;){ssize_t n=read(fd,scratch,capacity);if(n<0)return false;if(!n)break;b2_add(&hash,scratch,n);}
    b2_end(&hash,result);
    return lseek(fd,0,SEEK_SET)==0;
}
#endif
static bool spc_init(struct spc *c,const char *dir,const uint8_t source[32],
                     const uint8_t image[32],uint32_t rate,uint32_t pairs){
    uint8_t fmt[8];char hex[65];static const char digits[]="0123456789abcdef";
    struct b2_hash hash;b2_init(&hash);b2_add(&hash,"SM1 PCM s16le stereo 48000 v1",29);
    b2_store(fmt,rate);b2_store(fmt+4,pairs);b2_add(&hash,fmt,8);
    b2_add(&hash,source,32);b2_add(&hash,image,32);b2_end(&hash,c->key);
    for(unsigned i=0;i<32;i++){hex[2*i]=digits[c->key[i]>>4];hex[2*i+1]=digits[c->key[i]&15];}
    hex[64]=0;
    c->fd=-1;c->pairs=pairs;c->used=0;c->position=0;c->block=0;
    uint64_t blocks=((uint64_t)pairs+SPC_PAIRS-1)/SPC_PAIRS;
    if(!pairs||96+(uint64_t)pairs*4+blocks*32>INT32_MAX)return false;
    return snprintf(c->final,sizeof(c->final),"%s/%s.pcm",dir,hex)<(int)sizeof(c->final)
        && snprintf(c->tmp,sizeof(c->tmp),"%s/%s.part",dir,hex)<(int)sizeof(c->tmp);
}
static void spc_header(struct spc *c,uint8_t dst[SPC_HDR],bool commit){
    memcpy(dst,commit?"SPC1DONE":"SPC1DATA",8);memcpy(dst+8,c->key,32);
    b2_store(dst+40,c->pairs);b2_store(dst+44,SPC_PAIRS);
}
static void spc_digest(struct spc *c,unsigned ordinal,unsigned bytes,uint8_t digest[32]){
    uint8_t block[4];struct b2_hash h;b2_init(&h);b2_store(block,ordinal);
    b2_add(&h,c->key,32);b2_add(&h,block,4);b2_add(&h,c->pcm,bytes);b2_end(&h,digest);
}
static void spc_close(struct spc *c){if(c->fd>=0)close(c->fd);c->fd=-1;}
static bool spc_open(struct spc *c){
    uint8_t actual[SPC_HDR],wanted[SPC_HDR];
    c->fd=open(c->final,O_RDONLY);if(c->fd<0)return false;
    off_t end=SPC_HDR+(uint64_t)c->pairs*4+(((uint64_t)c->pairs+SPC_PAIRS-1)/SPC_PAIRS)*32;
    spc_header(c,wanted,false);
    bool ok=spc_io(c->fd,actual,sizeof(actual),false)&&!memcmp(actual,wanted,sizeof(actual));
    spc_header(c,wanted,true);
    ok=ok&&lseek(c->fd,0,SEEK_END)==end+(off_t)SPC_HDR&&lseek(c->fd,end,SEEK_SET)==end
        &&spc_io(c->fd,actual,sizeof(actual),false)&&!memcmp(actual,wanted,sizeof(actual));
    if(!ok)spc_close(c);
    return ok;
}
/* Return a checked slice until the end of its block. Random access reads one block. */
static unsigned spc_read(struct spc *c,uint32_t pair,const int16_t **samples){
    if(pair>=c->pairs||c->fd<0)return 0;
    unsigned ordinal=pair/SPC_PAIRS,skip=pair%SPC_PAIRS;
    uint32_t count=c->pairs-ordinal*SPC_PAIRS;if(count>SPC_PAIRS)count=SPC_PAIRS;
    off_t addr=SPC_HDR+(uint64_t)ordinal*(32+4*SPC_PAIRS);
    uint8_t actual[32],wanted[32];
    if(lseek(c->fd,addr,SEEK_SET)!=addr||!spc_io(c->fd,actual,32,false)||
       !spc_io(c->fd,c->pcm,4*count,false))return 0;
    spc_digest(c,ordinal,4*count,wanted);
    if(memcmp(actual,wanted,32))return 0;
    *samples=c->pcm+2*skip;return count-skip;
}
static void spc_abort(struct spc *c){spc_close(c);remove(c->tmp);}
static bool spc_begin(struct spc *c){
    uint8_t head[SPC_HDR];spc_header(c,head,false);
    c->fd=open(c->tmp,O_WRONLY|O_CREAT|O_TRUNC,0666);
    if(c->fd<0)return false;
    if(spc_io(c->fd,head,sizeof(head),true))return true;
    spc_abort(c);return false;
}
static bool spc_flush(struct spc *c){
    uint8_t sum[32];spc_digest(c,c->block,4*c->used,sum);
    if(!spc_io(c->fd,sum,32,true)||!spc_io(c->fd,c->pcm,4*c->used,true))return false;
    c->used=0;++c->block;return true;
}
static bool spc_append(struct spc *c,const int16_t *pcm,unsigned pairs){
    if(c->fd<0||pairs>c->pairs-c->position)return false;
    while(pairs){unsigned n=SPC_PAIRS-c->used;if(n>pairs)n=pairs;
        memcpy(c->pcm+2*c->used,pcm,n*4);c->used+=n;pcm+=n*2;pairs-=n;c->position+=n;
        if(c->used==SPC_PAIRS&&!spc_flush(c))return false;
    }
    return true;
}
static bool spc_publish(struct spc *c){
    uint8_t tail[SPC_HDR];spc_header(c,tail,true);
    bool ok=c->fd>=0&&c->position==c->pairs&&(!c->used||spc_flush(c));
    /* A committed marker is written only after every data record is durable. */
    ok=ok&&fsync(c->fd)==0&&spc_io(c->fd,tail,sizeof(tail),true)&&fsync(c->fd)==0;
    if(c->fd>=0&&close(c->fd))ok=false;
    c->fd=-1;
    if(ok&&rename(c->tmp,c->final)==0)return true;
    remove(c->tmp);return false;
}
#endif
