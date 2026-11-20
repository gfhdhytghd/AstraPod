/* Unkeyed BLAKE2s-256, with explicit little-endian loads and stores. */
#ifndef PODBOX_BLAKE2S_H
#define PODBOX_BLAKE2S_H
#include <stdint.h>
#include <string.h>
struct b2_hash { uint32_t h[8]; uint64_t bytes; uint8_t buf[64]; unsigned used; };
static const uint32_t b2_iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
static uint32_t b2_load(const uint8_t *p){
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void b2_store(uint8_t *p,uint32_t n){
    for(unsigned i=0;i<4;i++){p[i]=(uint8_t)n;n>>=8;}
}
static uint32_t b2_rotate(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void b2_g(uint32_t v[16],unsigned a,unsigned b,unsigned c,unsigned d,uint32_t x,uint32_t y){
    v[a]+=v[b]+x;v[d]=b2_rotate(v[d]^v[a],16);v[c]+=v[d];v[b]=b2_rotate(v[b]^v[c],12);
    v[a]+=v[b]+y;v[d]=b2_rotate(v[d]^v[a],8);v[c]+=v[d];v[b]=b2_rotate(v[b]^v[c],7);
}
static void b2_compress(struct b2_hash *s,int last){
    static const uint8_t order[10][16]={
        {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
        {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},{7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
        {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},{2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
        {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},{13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
        {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},{10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0}};
    uint32_t v[16],m[16];memcpy(v,s->h,32);memcpy(v+8,b2_iv,32);
    v[12]^=(uint32_t)s->bytes;v[13]^=(uint32_t)(s->bytes>>32);if(last)v[14]=~v[14];
    for(unsigned i=0;i<16;i++)m[i]=b2_load(s->buf+4*i);
    for(unsigned r=0;r<10;r++){
        const uint8_t *p=order[r];
        for(unsigned i=0;i<4;i++)b2_g(v,i,4+i,8+i,12+i,m[p[2*i]],m[p[2*i+1]]);
        for(unsigned i=0;i<4;i++)b2_g(v,i,4+(i+1)%4,8+(i+2)%4,12+(i+3)%4,m[p[8+2*i]],m[p[9+2*i]]);
    }
    for(unsigned i=0;i<8;i++)s->h[i]^=v[i]^v[i+8];
}
static void b2_init(struct b2_hash *s){
    memcpy(s->h,b2_iv,32);s->h[0]^=0x01010020;s->used=0;s->bytes=0;
}
static void b2_add(struct b2_hash *s,const void *input,size_t n){
    const uint8_t *p=input;
    while(n){if(s->used==64){s->bytes+=64;b2_compress(s,0);s->used=0;}
        size_t take=64-s->used;if(take>n)take=n;memcpy(s->buf+s->used,p,take);
        s->used+=take;p+=take;n-=take;}
}
static void b2_end(struct b2_hash *s,uint8_t out[32]){
    s->bytes+=s->used;memset(s->buf+s->used,0,64-s->used);b2_compress(s,1);
    for(unsigned i=0;i<8;i++)b2_store(out+4*i,s->h[i]);
}
#endif
