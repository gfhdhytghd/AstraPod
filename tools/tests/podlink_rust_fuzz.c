/* SPDX-License-Identifier: GPL-2.0-or-later
 * Deterministic differential fuzzing of the real C and Rust wire ABIs.
 */
#include "podlink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern unsigned c_encode(unsigned char *,unsigned,uint32_t,const void *,unsigned);
extern int c_decode_bytes(struct podlink_parser *,const unsigned char *,unsigned,unsigned *,struct podlink_message *);
extern int c_decode(struct podlink_parser *,unsigned char,struct podlink_message *);
void pfu_panic(void) {fputs("Rust panic\n",stderr);abort();}
static uint32_t seed=0x48f00d;
static unsigned random32(void) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static unsigned char stream[2*PODLINK_FRAME_MAX+128],payload[PODLINK_MAX_MESSAGE],expected[PODLINK_FRAME_MAX];
static void equal(int a,int b,const struct podlink_message *x,const struct podlink_message *y)
{
    assert(a==b);
    if(a==1)assert(x->type==y->type && x->flags==y->flags && x->seq==y->seq && x->length==y->length && !memcmp(x->data,y->data,x->length));
}
static unsigned long calls;
static void compare(unsigned size,unsigned chunk,bool single)
{
    static struct podlink_parser a,b;memset(&a,0,sizeof a);memset(&b,0,sizeof b);
    struct podlink_message x,y;unsigned pos=0;
    for(unsigned turns=0;;turns++) {
        assert(turns<size*4+100);calls++;
        unsigned n=(size-pos<chunk?size-pos:chunk);
        unsigned u=0,v=0;int rc,rd;
        if(single && n) {rc=c_decode(&a,stream[pos],&x);rd=podlink_decode(&b,stream[pos],&y);u=v=1;}
        else {rc=c_decode_bytes(&a,stream+pos,n,&u,&x);rd=podlink_decode_bytes(&b,stream+pos,n,&v,&y);}
        equal(rc,rd,&x,&y);assert(u==v && a.used==b.used && a.consumed==b.consumed && !memcmp(a.bytes,b.bytes,a.used));
        pos+=u;if(pos==size && !rc)break;
    }
}
int main(void)
{
    for(unsigned test=0;test<10000;test++) {
        unsigned len=random32()%(PODLINK_MAX_MESSAGE+1),type=1+random32()%7,seq=random32();
        for(unsigned i=0;i<len;i++)payload[i]=(unsigned char)random32();
        unsigned n=c_encode(stream,type,seq,payload,len);
        assert(n==podlink_encode(expected,type,seq,payload,len) && !memcmp(stream,expected,n));
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
        const char *record=records[test%8];n+=c_encode(stream+n,test%8>=6?3:2,seq+1,record,strlen(record));
        compare(n,1+random32()%4096,false);
        if(test<64)compare(n,1,true);
    }
    printf("PASS differential fuzz: 10000 seeded streams, %lu decode calls; outputs, consumed bytes, parser state and CRC encoding identical\n",calls);
}
