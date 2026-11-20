/* Bounded, read-only parser for a single unedited fragmented FLAC sound track.
 * This header is shared by the metadata reader and the resident codec route. */
#ifndef SM1_FMP4_H
#define SM1_FMP4_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define SM4_TAG(a,b,c,d) ((uint32_t)(a)<<24|(b)<<16|(c)<<8|(d))
struct sm4_box { uint32_t start, end, type; };
struct sm4_info { uint8_t stream[34]; uint32_t track, rate, total, first; };
struct sm4_packet { uint32_t offset, sample; uint16_t size, duration; };
static uint32_t sm4_u32(const uint8_t *b)
{ return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3]; }
#ifdef SM4_PACKET_TABLE
static uint32_t sm4_index_bytes;
#endif
static bool sm4_read(int fd,uint32_t pos,void *dst,size_t n)
{
#ifdef SM4_PACKET_TABLE
    sm4_index_bytes+=n;
#endif
    return lseek(fd,pos,SEEK_SET)==(off_t)pos && read(fd,dst,n)==(ssize_t)n;
}
static bool sm4_box_at(int fd,uint32_t pos,uint32_t end,struct sm4_box *out)
{
    uint8_t b[8];
    if (pos>end || end-pos<8 || !sm4_read(fd,pos,b,8)) return false;
    uint32_t size=sm4_u32(b);
    if (size<8 || size>end-pos) return false;
    *out=(struct sm4_box){pos,pos+size,sm4_u32(b+4)}; return true;
}
/* Require exactly one such child. A malformed sibling invalidates the list. */
static bool sm4_child(int fd,struct sm4_box parent,uint32_t skip,uint32_t type,struct sm4_box *found)
{
    uint32_t pos=parent.start+8;
    if (skip>parent.end-pos) return false;
    pos+=skip; bool seen=false; unsigned boxes=0;
    while (pos<parent.end) {
        struct sm4_box b;
        if (++boxes>128 || !sm4_box_at(fd,pos,parent.end,&b)) return false;
        if (b.type==type) { if (seen) return false; seen=true; *found=b; }
        if (b.type==SM4_TAG('e','d','t','s')) return false;
        pos=b.end;
    }
    return seen;
}
static bool sm4_payload(int fd,struct sm4_box box,uint32_t skip,void *dst,size_t n)
{
    uint32_t bytes=box.end-box.start-8;
    return skip<=bytes && n<=bytes-skip && sm4_read(fd,box.start+8+skip,dst,n);
}
static bool sm4_identify(int fd,uint32_t end,struct sm4_info *info)
{
    struct sm4_box b,tr,md,st,leaf;
    uint8_t v[8]; uint32_t pos=0;
    if (!sm4_box_at(fd,pos,end,&b) || b.type!=SM4_TAG('f','t','y','p')) return false;
    pos=b.end;
    if (!sm4_box_at(fd,pos,end,&b) || b.type!=SM4_TAG('m','o','o','v')) return false;
    if (!sm4_child(fd,b,0,SM4_TAG('t','r','a','k'),&tr) ||
        !sm4_child(fd,b,0,SM4_TAG('m','v','e','x'),&leaf) ||
        !sm4_child(fd,leaf,0,SM4_TAG('t','r','e','x'),&leaf) ||
        !sm4_payload(fd,leaf,0,v,8) || sm4_u32(v)) return false;
    info->track=sm4_u32(v+4); info->first=b.end;
    if (!info->track || !sm4_child(fd,tr,0,SM4_TAG('t','k','h','d'),&leaf) ||
        !sm4_payload(fd,leaf,0,v,1) || v[0] || !sm4_payload(fd,leaf,12,v,4) ||
        sm4_u32(v)!=info->track || !sm4_child(fd,tr,0,SM4_TAG('m','d','i','a'),&md) ||
        !sm4_child(fd,md,0,SM4_TAG('h','d','l','r'),&leaf) ||
        !sm4_payload(fd,leaf,8,v,4) || sm4_u32(v)!=SM4_TAG('s','o','u','n') ||
        !sm4_child(fd,md,0,SM4_TAG('m','d','h','d'),&leaf) ||
        !sm4_payload(fd,leaf,0,v,1) || v[0] || !sm4_payload(fd,leaf,12,v,4)) return false;
    uint32_t timescale=sm4_u32(v);
    if (!sm4_child(fd,md,0,SM4_TAG('m','i','n','f'),&st) ||
        !sm4_child(fd,st,0,SM4_TAG('s','t','b','l'),&st) ||
        !sm4_child(fd,st,0,SM4_TAG('s','t','s','d'),&st) ||
        !sm4_payload(fd,st,0,v,8) || sm4_u32(v) || sm4_u32(v+4)!=1 ||
        !sm4_child(fd,st,8,SM4_TAG('f','L','a','C'),&st) ||
        !sm4_payload(fd,st,8,v,2) || v[0] || v[1] ||
        !sm4_child(fd,st,28,SM4_TAG('d','f','L','a'),&leaf) ||
        !sm4_payload(fd,leaf,0,v,8) || sm4_u32(v) || sm4_u32(v+4)!=0x80000022u ||
        !sm4_payload(fd,leaf,8,info->stream,34)) return false;
    const uint8_t *s=info->stream;
    info->rate=(uint32_t)s[10]<<12 | s[11]<<4 | s[12]>>4;
    info->total=sm4_u32(s+14);
    return info->rate==timescale && info->rate && !(s[13]&15) && info->total;
}
#ifdef SM4_PACKET_TABLE
static bool sm4_index(int fd,uint32_t end,const struct sm4_info *info,struct sm4_packet *p,
                      uint32_t limit,uint32_t *count)
{
    uint32_t pos=info->first, sample=0, used=0;
    while (pos<end) {
        struct sm4_box moof,mdat,traf,tfhd,tfdt,trun;
        uint8_t h[20], entry[8];
        if (!sm4_box_at(fd,pos,end,&moof) || moof.type!=SM4_TAG('m','o','o','f') ||
            !sm4_box_at(fd,moof.end,end,&mdat) || mdat.type!=SM4_TAG('m','d','a','t') ||
            !sm4_child(fd,moof,0,SM4_TAG('t','r','a','f'),&traf) ||
            !sm4_child(fd,traf,0,SM4_TAG('t','f','h','d'),&tfhd) ||
            !sm4_payload(fd,tfhd,0,h,20) || sm4_u32(h)!=0x2002a ||
            sm4_u32(h+4)!=info->track || sm4_u32(h+8)!=1) return false;
        uint32_t default_duration=sm4_u32(h+12);
        if (!sm4_child(fd,traf,0,SM4_TAG('t','f','d','t'),&tfdt) ||
            !sm4_payload(fd,tfdt,0,h,8) || sm4_u32(h) || sm4_u32(h+4)!=sample ||
            !sm4_child(fd,traf,0,SM4_TAG('t','r','u','n'),&trun) ||
            !sm4_payload(fd,trun,0,h,12)) return false;
        uint32_t flags=sm4_u32(h), n=sm4_u32(h+4), off=mdat.start+8;
        if ((flags!=0x201 && flags!=0x301) || sm4_u32(h+8)!=off-moof.start ||
            !n || n>limit-used) return false;
        unsigned stride=flags==0x301?8:4;
        if (trun.end-trun.start-20!=n*stride) return false;
        for (uint32_t i=0;i<n;i++) {
            if (!sm4_payload(fd,trun,12+i*stride,entry,stride)) return false;
            uint32_t dur=stride==8?sm4_u32(entry):default_duration;
            uint32_t size=sm4_u32(entry+stride-4);
            if (!dur || dur>4608 || dur>info->total-sample ||
                !size || size>32768 || size>mdat.end-off) return false;
            p[used++]=(struct sm4_packet){off,sample,size,dur};
            off+=size; sample+=dur;
        }
        if (off!=mdat.end) return false;
        pos=mdat.end;
    }
    *count=used;
    return used && sample==info->total;
}
#endif
#endif
