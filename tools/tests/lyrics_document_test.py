"""Real lyrics document/parser on a 32-bit host ABI; no device or audio."""
from pathlib import Path
import re,subprocess,sys
r=Path(__file__).resolve().parents[2];o=Path(sys.argv[1]).resolve();o.mkdir(parents=True,exist_ok=True)
s=(r/'apps-ipod/viewers/lyric_viewer/lyrics.c').read_text()
s=re.sub(r'^#include ["].*\n','',s,flags=re.M)
h=(r/'apps-ipod/viewers/lyric_viewer/lyrics.h').read_text();h=re.sub(r'^#include "metadata.h".*\n','',h,flags=re.M)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define MAX_PATH 260
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#define UTF_8 1
#define ISO_8859_1 0
#define NUM_CODEPAGES 2
#define ID3_VER_2_2 2
#define ID3_VER_2_3 3
#define ID3_VER_2_4 4
#define AFMT_MPA_L1 1
#define AFMT_MPA_L2 2
#define AFMT_MPA_L3 3
#define AFMT_MP4_ALAC 4
#define AFMT_MP4_AAC 5
#define AFMT_MP4_AAC_HE 6
#define AFMT_FLAC 7
#define AFMT_OGG_VORBIS 8
#define AFMT_UNKNOWN 0
#define AFMT_SPEEX 9
#define AFMT_OPUS 10
struct mp3entry {char path[MAX_PATH];const char *title,*artist;unsigned long length,id3v2len;unsigned int codectype;unsigned char id3version;};
struct ogg_file {int fd;bool packet_ended;long packet_remaining;};

static off_t ffilesize(int fd){struct stat st;return fstat(fd,&st)?-1:st.st_size;}
static uint32_t get_long_be(const void *buf){
    const unsigned char *p=buf;
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
#ifndef LYRICS_EMBEDDED_FIXTURES
static long find_mp4_lyrics(int fd){(void)fd;return 0;}
static long find_vorbis_lyrics(struct ogg_file *file,int fd,int type){(void)file;(void)fd;(void)type;return 0;}
static ssize_t ogg_file_read(struct ogg_file *file,void *buf,size_t count){return read(file->fd,buf,count);}
static int id3_unsynchronize(char *tag,int len,bool *ff){(void)tag;(void)ff;return len;}
#endif
static struct {int default_codepage;} global_settings;
static const int buflib_ops_locked;
static unsigned char arena[32768] __attribute__((aligned(16)));
static bool fail_alloc;
static int allocs,frees;
static int core_alloc_ex(size_t n,const void *ops){(void)ops;assert(n==sizeof arena);if(fail_alloc)return -1;allocs++;return 1;}
static void *core_get_data(int h){assert(h==1);return arena;}
static void core_free(int h){assert(h==1);frees++;}

static bool file_exists(const char *p){struct stat st;return !stat(p,&st);}
static void fix_path_part(char *p,int start,int end){(void)p;(void)start;(void)end;}
static int read_line(int fd,char *b,int size){int n=0;char c;while(read(fd,&c,1)==1){if(c=='\n')break;if(n<size-1)b[n++]=c;}b[n]=0;return n;}
static unsigned char *utf8encode(unsigned long c, unsigned char *out){
    if(c<0x80)*out++=c;
    else if(c<0x800){*out++=0xc0|(c>>6);*out++=0x80|(c&63);}
    else if(c<0x10000){*out++=0xe0|(c>>12);*out++=0x80|((c>>6)&63);*out++=0x80|(c&63);}
    else{*out++=0xf0|(c>>18);*out++=0x80|((c>>12)&63);*out++=0x80|((c>>6)&63);*out++=0x80|(c&63);}
    return out;
}
static unsigned char *iso_decode_ex(const unsigned char*s,unsigned char*d,int enc,int n,int cap){
    unsigned char *limit=d+cap;
    while(n-- && d<limit){
        if(enc==ISO_8859_1 && *s>=128){if(limit-d<2)break;d=utf8encode(*s++,d);}
        else *d++=*s++;
    }
    return d;
}
'''
post=r'''
static void writefile(const char *path,const char *text){FILE*f=fopen(path,"w");assert(f);fputs(text,f);fclose(f);}
int main(int argc,char**argv){assert(argc==2);assert(!chdir(argv[1]));
 assert(lyrics_begin_text()==LYRICS_OK&&!lyrics_timed());
 char transient[]="First\n让音乐陪你\r\nThird";
 assert(lyrics_append_text(transient));memset(transient,'X',sizeof transient-1);
 assert(lyrics_finish_text()&&lyrics_loaded()&&lyrics_count()==3);
 assert(!strcmp(lyrics_text(0),"First")&&!strcmp(lyrics_text(1),"让音乐陪你"));
 assert(lyrics_word_count(1)==1&&lyrics_word_time(1,0)==-1);
 assert(!lyrics_text(-1)&&!lyrics_text(3));
 assert(lyrics_begin_text()==LYRICS_OK&&allocs==1);
 for(int i=0;i<75;i++){char b[40];snprintf(b,sizeof b,"Remote row %d",i);assert(lyrics_append_text(b));}
 assert(lyrics_finish_text()&&lyrics_count()==75&&!strcmp(lyrics_text(74),"Remote row 74"));
 assert(lyrics_begin_text()==LYRICS_OK);char large[125];memset(large,'a',124);large[124]=0;
 int lines=0;while(lyrics_append_text(large))lines++;
 assert(lines>100&&lines<300&&lyrics_finish_text()&&lyrics_count()==lines);
 for(int i=0;i<lines;i++)assert(strlen(lyrics_text(i))==124);
 struct mp3entry track={.length=60000};snprintf(track.path,sizeof track.path,"%s/song.wav",argv[1]);
 writefile("song.txt","Local plain text\n中文歌词\nLast");
 unlink("song.lrc"); /* a previous run's timed fixture has higher priority */
 assert(lyrics_load(&track)==LYRICS_OK&&!lyrics_timed()&&lyrics_count()==3);
 assert(!strcmp(lyrics_text(0),"Local plain text")&&!strcmp(lyrics_text(1),"中文歌词"));
 writefile("song.lrc","[offset:100]\n[00:01.00]One\n[00:02.00]<00:02.00>Two <00:03.00>words\n[00:04.00]\n[00:05.00]Last\n");
 assert(lyrics_load(&track)==LYRICS_OK&&lyrics_timed()&&lyrics_count()==4);
 assert(lyrics_index_at(1099)==-1&&lyrics_index_at(1100)==0&&lyrics_index_at(2100)==1);
 assert(lyrics_time(1)==2100&&lyrics_end_time(1)==4100);
 assert(!strcmp(lyrics_text(1),"Two words")&&lyrics_word_count(1)==2);
 assert(lyrics_word_time(1,0)==2100&&lyrics_word_time(1,1)==3100);
 assert(!strcmp(lyrics_word_text(1,1),"words"));
 lyrics_close();assert(frees==1&&!lyrics_loaded()&&lyrics_count()==0);
 fail_alloc=true;assert(lyrics_begin_text()==LYRICS_NO_MEMORY);assert(lyrics_load(&track)==LYRICS_NO_MEMORY);
 assert(lyrics_load(NULL)==LYRICS_NO_TRACK);
 puts("PASS production text/enhanced LRC model: UTF-8, copy lifetime, 75 remote rows, bounds, buffer exhaustion, reuse/release, no-memory");
}
'''
p=o/'document.c';p.write_text(pre+h+s+post)
subprocess.run(['cc','-m32','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(p),'-o',str(o/'document')],check=True)
subprocess.run([str(o/'document'),str(o)],check=True)
