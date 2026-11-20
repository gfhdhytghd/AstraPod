#ifndef SM1_FLAC_PROFILE_H
#define SM1_FLAC_PROFILE_H

struct sm1_flac_profile {
    unsigned rate, depth, decimation, max_block, max_packet;
    const char *image;
};

/* Names select separately pinned and independently replaceable firmware data. */
static const struct sm1_flac_profile sm1_profiles[] = {
    {44100, 16, 1,  4608, 32768, "codec44.x17i"},
    {48000, 16, 1,  4608, 32768, "codec48.x17i"},
    {44100, 24, 1,  4608, 32768, "depth24-44.x17i"},
    {48000, 24, 1,  4608, 32768, "depth24-48.x17i"},
    {96000, 24, 2,  8192, 65535, "ahead96.x17i"},
    {192000,24, 4, 16384, 65535, "ahead192.x17i"},
};

static inline const struct sm1_flac_profile *sm1_profile_for(unsigned rate, unsigned depth)
{
    for (unsigned i=0; i<sizeof(sm1_profiles)/sizeof(sm1_profiles[0]); ++i)
        if (sm1_profiles[i].rate == rate && sm1_profiles[i].depth == depth)
            return &sm1_profiles[i];
    return 0;
}
static inline unsigned sm1_output_pairs(unsigned source, unsigned factor)
{
    return source/factor + (source%factor != 0);
}
#endif
