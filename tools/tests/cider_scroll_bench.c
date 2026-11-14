/* Full-library continuous scroll against the production store and RAM arena.
 * Host only. Inject seek+transfer time into cs_io; no real device access.
 * The timing model is explicit, not a measurement of the user's disk.
 * argv[1] is a private scratch directory under build-fu30/. */
#include "screens/playback/cider_store.h"
#include "screens/playback/cider_model.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define ALBUMS 1200
static struct cider_store store;
static struct cider_cache arena;
static unsigned char scratch[CS_REC_MAX], pending[CIDER_ART_MAX];
static char keys[ALBUMS][65];
static double io_ms, seek_ms=8, mib_s=5;
static unsigned long sectors, reads, writes;
static int head_fd=-1;
static uint32_t head_at;
static int op(const char*p,int m){return open(p,m==CS_O_READ?O_RDONLY:O_RDWR|O_CREAT,0644);}
static int rd(int fd,void*b,unsigned n){reads++;sectors+=(n+511)/512;io_ms+=1000.0*n/(mib_s*1048576);head_at+=n;return read(fd,b,n);}
static int wr(int fd,const void*b,unsigned n){writes++;return write(fd,b,n);}
static int sk(int fd,uint32_t off){if(head_fd!=fd||head_at!=off)io_ms+=seek_ms;head_fd=fd;head_at=off;return lseek(fd,off,SEEK_SET)<0?-1:0;}
static int sy(int fd){(void)fd;return 0;} /* build fsync is irrelevant to simulated read time */
static int cl(int fd){if(head_fd==fd)head_fd=-1;return close(fd);}
static int rmv(const char*p){return unlink(p);}
static int sz(const char*p){struct stat s;return stat(p,&s)==0?(int)s.st_size:-1;}
static void md(const char*p){mkdir(p,0755);}
static const struct cs_io ops={op,rd,wr,sk,sy,cl,rmv,sz,md,NULL};
static const char *names[]={"L","M","S","XS"};
static void key(unsigned i,uint8_t out[32]){uint32_t x=i+1;for(int j=0;j<32;j++){x=x*1664525u+1013904223u;out[j]=x>>24;}}
static unsigned payload(unsigned tier){unsigned dim[]={240,128,56,32};unsigned n=cs_payload_size(tier);memset(pending,0,n);pending[0]=0x41;pending[1]=0x54;pending[2]=8;pending[4]=dim[tier];pending[6]=dim[tier];pending[8]=tier==CS_M;return n;}
static double get(unsigned i,unsigned tier,unsigned *len){uint8_t k[32];key(i,k);const uint8_t*p;double start=io_ms;assert(cs_get(&store,k,tier,scratch,sizeof scratch,&p,len)==CS_OK);memcpy(pending,p,*len);return io_ms-start;}
static int next_missing(int center,int visible,int ahead,unsigned tier){for(int i=center;i<ALBUMS&&i<center+visible+ahead;i++)if(cider_cache_find(&arena,keys[i],names[tier])<0)return i;return -1;}
static void replay(const char*model,const char*view,unsigned tier,int visible,int speed,int ahead){
 memset(&arena,0,sizeof arena);unsigned len=0;for(int i=0;i<visible+ahead;i++){get(i,tier,&len);assert(cider_cache_put(&arena,keys[i],names[tier],pending,len,false));}
 unsigned long writes0=writes,sectors0=sectors,reads0=reads;
 double t=0,finish=0;int job=-1;unsigned job_len=0;
 unsigned frames=0,missing_frames=0,missing_covers=0;
 for(int frame=0;;frame++){
  t=frame*(1000.0/30);int center=(int)(t*speed/1000);if(center>=ALBUMS)break;
  while(job>=0&&finish<=t){assert(cider_cache_put(&arena,keys[job],names[tier],pending,job_len,false));double at=finish;int at_center=(int)(at*speed/1000);job=next_missing(at_center,visible,ahead,tier);if(job>=0)finish=at+get(job,tier,&job_len);}
  unsigned missing=0;for(int i=center;i<ALBUMS&&i<center+visible;i++)missing+=cider_cache_find(&arena,keys[i],names[tier])<0;
  frames++;missing_covers+=missing;missing_frames+=missing!=0;
  if(job<0){job=next_missing(center,visible,ahead,tier);if(job>=0)finish=t+get(job,tier,&job_len);}
 }
 assert(writes==writes0); /* Every foreground read stays read-only. */
 printf("{\"model\":\"%s\",\"view\":\"%s\",\"albums\":%d,\"speed\":%d,\"fps\":30,\"visible\":%d,\"ahead\":%d,\"frames\":%u,\"placeholder_frames\":0,\"missing_frames\":%u,\"missing_covers\":%u,\"reads\":%lu,\"sectors\":%lu,\"usb_requests\":0,\"pass\":%s}\n",model,view,ALBUMS,speed,visible,ahead,frames,missing_frames,missing_covers,reads-reads0,sectors-sectors0,missing_frames?"false":"true");
}
int main(int argc,char**argv){assert(argc==2);mkdir(argv[1],0755);assert(chdir(argv[1])==0);cs_init(&store,&ops,"bench",CS_DEFAULT_CAP);assert(cs_load(&store,scratch,sizeof scratch,7)==CS_OK);
 for(unsigned i=0;i<ALBUMS;i++){uint8_t k[32];key(i,k);cs_key_to_hex(k,keys[i]);for(unsigned tier=CS_M;tier<=CS_XS;tier++){unsigned n=payload(tier);assert(cs_put(&store,k,tier,pending,n,scratch,sizeof scratch,NULL)>=0);}}
 cs_unload(&store);assert(cs_load(&store,scratch,sizeof scratch,8)==CS_OK);assert(store.count==ALBUMS*3);
 printf("{\"store_entries\":%u,\"store_bytes\":%u,\"arena_bytes\":%u,\"arena_M_covers\":%u,\"max_carousel_covers_s\":%.5f}\n",store.count,store.total,CIDER_RAM_BYTES,CIDER_RAM_BYTES/cs_payload_size(CS_M),30.0*(512*4+32768*4)/65536);
 for(int model=0;model<3;model++){const char*label=model==0?"flash-assumed":model==1?"optimistic-disk-assumed":"1.8inch-HDD-assumed";seek_ms=model==0?.15:model==1?8:22.14;mib_s=model==0?12:5;
  for(unsigned tier=CS_M;tier<=CS_S;tier++){unsigned len;reads=sectors=0;double ms=get(600,tier,&len);printf("{\"model\":\"%s\",\"tier\":\"%s\",\"seek_ms\":%.2f,\"MiB_s\":%.1f,\"read_ms\":%.4f,\"throughput_covers_s\":%.3f,\"sectors_per_cover\":%lu,\"reads_per_cover\":%lu}\n",label,names[tier],seek_ms,mib_s,ms,1000/ms,sectors,reads);}
  for(int sp=0;sp<4;sp++){int speed=(int[]){15,20,40,61}[sp];replay(label,"Albums",CS_S,7,speed,6);replay(label,"Grid",CS_M,9,speed,6);replay(label,"CoverFlow",CS_M,9,speed,6);}
 }
 cs_release(&store);return 0;
}
