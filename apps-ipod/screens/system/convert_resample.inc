/* Bandlimited fractional resampling. A Blackman-windowed sinc prototype
 * has 16 zero crossings either side. Each phase is normalized to unity DC.
 * Delay is compensated by retaining right context until EOF. */
#include "convert_sinc.h"
#define RS_PHASES 32
#define RS_TAPS 576
static struct {
    int32_t coeff[RS_PHASES+1][RS_TAPS];
    int32_t ring[1024][2];
    unsigned rate,radius,taps,phase;
    uint64_t input,center,emitted;
} rs;

static bool rs_init(unsigned rate)
{
    if (rate<=48000 || rate>768000) return false;
    memset(&rs,0,sizeof rs);
    rs.rate=rate; rs.radius=(16*rate+43499)/43500;
    rs.taps=2*rs.radius+1;
    for (unsigned phase=0;phase<=RS_PHASES;++phase) {
        int sum=0;
        for (unsigned i=0;i<rs.taps;++i) {
            int dist=(int)((i-rs.radius)*RS_PHASES)-(int)phase;
            uint64_t x=(uint64_t)(dist<0?-dist:dist)*43500*256;
            unsigned den=rate*RS_PHASES;
            unsigned index=x/den;
            int c=0;
            if(index<4096) {
                int diff=convert_sinc[index+1]-convert_sinc[index];
                c=convert_sinc[index]+(int64_t)diff*(int64_t)(x%den)/den;
            }
            rs.coeff[phase][i]=c;sum+=c;
        }
        int total=0;
        for (unsigned i=0;i<rs.taps;++i) {
            int c=(int64_t)rs.coeff[phase][i]*1048576/sum;
            rs.coeff[phase][i]=c;total+=c;
        }
        rs.coeff[phase][rs.radius]+=1048576-total;
    }
    return true;
}

static bool rs_emit(struct flac_output *out)
{
    unsigned scaled=rs.phase*RS_PHASES;
    unsigned phase=scaled/48000;
    int fraction=(scaled%48000)*256/48000;
    int64_t acc[2]={0,0};
    for (unsigned i=0;i<rs.taps;++i) {
        int64_t at=(int64_t)rs.center+i-rs.radius;
        if (at<0 || (uint64_t)at>=rs.input) continue;
        int c=rs.coeff[phase][i];
        c+=((rs.coeff[phase+1][i]-c)*fraction)>>8;
        for (int ch=0;ch<2;++ch) acc[ch]+=(int64_t)c*rs.ring[at&1023][ch];
    }
    int16_t pair[2];
    for(int ch=0;ch<2;++ch) {
        int v=(acc[ch]+(1<<27))>>28;
        pair[ch]=v<-32768?-32768:v>32767?32767:v;
    }
    rs.phase+=rs.rate;rs.center+=rs.phase/48000;rs.phase%=48000;++rs.emitted;
    return flac_out_add(out,pair,1);
}
