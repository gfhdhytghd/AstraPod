/* SPDX-License-Identifier: GPL-2.0-or-later
 * Deterministic 10000-stream corpus. Hash every codec observation per seed.
 * FU49 fixtures were recorded with the retired C implementation.
 */
#include "podlink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
static EVP_MD_CTX *digest;
static void record(const void *p,unsigned n) {assert(EVP_DigestUpdate(digest,p,n)==1);}
static void number(uint32_t n) {record(&n,sizeof n);}
void pfu_panic(void) {fputs("Rust panic\n",stderr);abort();}
static uint32_t seed=0x48f00d;
static unsigned random32(void) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static unsigned char stream[2*PODLINK_FRAME_MAX+128],payload[PODLINK_MAX_MESSAGE];
static unsigned long calls;
static void compare(unsigned size,unsigned chunk,bool single)
{
    static struct podlink_parser b;memset(&b,0,sizeof b);
    struct podlink_message y;unsigned pos=0;
    for(unsigned turns=0;;turns++) {
        assert(turns<size*4+100);calls++;
        unsigned n=(size-pos<chunk?size-pos:chunk);
        unsigned v=0;int rd;
        if(single && n) {rd=podlink_decode(&b,stream[pos],&y);v=1;}
        else {rd=podlink_decode_bytes(&b,stream+pos,n,&v,&y);}
        number(rd);number(v);number(b.used);number(b.consumed);record(b.bytes,b.used);
        if(rd==1){number(y.type);number(y.flags);number(y.seq);number(y.length);record(y.data,y.length);}
        pos+=v;if(pos==size && !rd)break;
    }
}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *out=fopen(argv[1],"wb");assert(out);
    digest=EVP_MD_CTX_new();assert(digest);
    for(unsigned test=0;test<10000;test++) {
        uint32_t initial_seed=seed;assert(EVP_DigestInit_ex(digest,EVP_sha256(),NULL)==1);number(seed);
        unsigned len=random32()%(PODLINK_MAX_MESSAGE+1),type=1+random32()%7,seq=random32();
        for(unsigned i=0;i<len;i++)payload[i]=(unsigned char)random32();
        unsigned n=podlink_encode(stream,type,seq,payload,len);
        record(stream,n);
        switch(test%8) {
        case 0:stream[random32()%n]^=1u<<(random32()%8);break;
        case 1:memset(stream+12,255,4);break;
        case 2:stream[5]=1;break;
        case 3:n=random32()%n;break;
        case 4:memmove(stream+7,stream,n);memcpy(stream,"garbage",7);n+=7;break;
        case 5:if(n>32){memcpy(stream+17,"PLNK",4);stream[21]=2;}break;
        default:break;
        }
        // Always follow corrupt input with a valid application-shaped frame.
        const char *records[]={"HR\t1\thost","W\t1\nw1\t-\t1\t0\t0\t0\tCached","CA\t1\tplaying","CL\t1\t0\t1\tSong","DA\t1\t0123456789abcdef0123456789abcdef","DK\t1\t0123456789abcdef0123456789abcdef","CAR1\1\2\3","CXS1\1\2\3"};
        const char *item=records[test%8];n+=podlink_encode(stream+n,test%8>=6?3:2,seq+1,item,strlen(item));
        number(n);record(stream,n);compare(n,1+random32()%4096,false);
        if(test<64)compare(n,1,true);
        unsigned char hash[32];unsigned hash_len;
        assert(EVP_DigestFinal_ex(digest,hash,&hash_len)==1 && hash_len==32);
        assert(fwrite(&initial_seed,4,1,out)==1 && fwrite(hash,32,1,out)==1);
    }
    EVP_MD_CTX_free(digest);assert(!fclose(out));
    printf("PASS golden fuzz: 10000 seeded streams, %lu decode calls; SHA256 per seed covers outputs, consumed bytes, parser state and CRC encoding\n",calls);
}
