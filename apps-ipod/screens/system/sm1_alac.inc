/* Shared by the ALAC codec on ordinary playback and during conversion. */
static unsigned sm1_alac_depth;

bool sm1_alac_begin(unsigned depth, unsigned channels, unsigned pb,
                    unsigned kb, unsigned mb)
{
    if ((depth != 16 && depth != 24) || channels < 1 || channels > 2 ||
        pb != 40 || kb != 14 || mb != 10)
        return false;
    const char *path = depth == 16 ? SM1_TEST_DIR "/alac16.x17i" : SM1_TEST_DIR "/alac24.x17i";
    int image = open(path, O_RDONLY);
    if (image < 0)
        return false;
    mutex_lock(&sm1_owner);
    bool ready = sm1_test_image(image, -1) && sm1_begin(image);
    close(image);
    if (!ready)
    {
        sm1_end();
        mutex_unlock(&sm1_owner);
        return false;
    }
    sm1_alac_depth = depth;
    SM1_PARAMS[2] = depth;
    SM1_PARAMS[3] = pb;
    SM1_PARAMS[4] = kb;
    SM1_PARAMS[5] = mb;
    return true;
}

bool sm1_alac_frame(const unsigned char *packet, size_t size, unsigned samples,
                    int32_t *left, int32_t *right)
{
    if (!packet || !size || size > SM1_PACKET_MAX || !samples || samples > 4096)
        return false;
    memcpy(sm1_packet, packet, size);
    return sm1_packet_output(size, samples * 2, sm1_alac_depth, left, right);
}

void sm1_alac_end(void)
{
    debug_log(DEBUG_LOG_PLAYBACK, "SM1 ALAC: %u packets%s", hw.count,
              hw.fault ? ", decoder fault" : "");
    sm1_end();
    mutex_unlock(&sm1_owner);
}
