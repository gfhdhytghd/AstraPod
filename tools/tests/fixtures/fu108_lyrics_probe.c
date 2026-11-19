/* Included only in a scratch SDL viewer. Exercise its real framebuffer. */
#ifdef SIMULATOR
#define PROBE_DOT 1
#define PROBE_TEXT 2
#define PROBE_ROWS 32
#define PROBE_VOICES 4
static uint32_t fu108_clock;
static unsigned char probe_pixels[LV_PIXELS], stable_dots[LV_PIXELS];
static int probe_kind, probe_vocal = -1;
static unsigned probe_peak[PROBE_VOICES], probe_ink[PROBE_VOICES];
static int probe_baseline[PROBE_ROWS];
static unsigned probe_sizes[PROBE_ROWS];

static void probe_expect(bool ok, const char *what, unsigned sample)
{
    if (!ok)
    {
        fprintf(stderr, "FU108 assertion: %s (sample %u)\n", what, sample);
        abort();
    }
}

static void probe_pixel(fb_data *dst, unsigned coverage)
{
    if (!fu108_clock || !probe_kind || !coverage)
        return;
    unsigned pixel = dst - lv_frame();
    probe_expect(pixel < LV_PIXELS, "framebuffer bounds", pixel);
    probe_pixels[pixel] |= probe_kind;
    if (probe_vocal >= 0)
    {
        unsigned green = (*dst >> 5) & 63u;
        probe_peak[probe_vocal] = MAX(probe_peak[probe_vocal], green);
        probe_ink[probe_vocal] += green;
    }
}

static void probe_row(int line, int row, bool background, int font, int y, int alpha)
{
    if (!fu108_clock)
        return;
    probe_kind = PROBE_TEXT;
    probe_vocal = line < 2 ? 2 * line + background : -1;
    if (!lv.mode.manual || background || row || !alpha)
        return;
    int baseline = y + font_get(font)->ascent;
    if (probe_baseline[line] != INT_MIN)
        probe_expect(probe_baseline[line] == baseline, "native faces share a baseline", line);
    probe_baseline[line] = baseline;
    probe_sizes[line] |= font == lv.large_font ? 2 : 1;
}

static void probe_dump(const char *dir, const char *name)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s.raw", dir, name);
    FILE *out = fopen(path, "wb");
    probe_expect(out != NULL, "open screenshot", 0);
    probe_expect(fwrite(lv_frame(), LV_FRAME, 1, out) == 1, "save screenshot", 0);
    fclose(out);
}

static void probe_draw(void)
{
    memset(probe_pixels, 0, sizeof(probe_pixels));
    memset(probe_peak, 0, sizeof(probe_peak));
    memset(probe_ink, 0, sizeof(probe_ink));
    memset(probe_sizes, 0, sizeof(probe_sizes));
    for (unsigned i = 0; i < PROBE_ROWS; ++i)
        probe_baseline[i] = INT_MIN;
    lv_draw(true, LV_UPDATE);
}

static void fu108_lyrics_probe(const char *dir)
{
    probe_expect(lyrics_ttml() && lyrics_count() == 8 && lv.masks, "loaded TTML and masks", 0);
    probe_expect(font_filename_matches_loaded_id(lv.font, FONT_DIR "/18-noto-sans.pbf") &&
                 font_filename_matches_loaded_id(lv.large_font, FONT_DIR "/20-noto-sans.pbf") &&
                 font_filename_matches_loaded_id(lv.small_font, FONT_DIR "/14-noto-sans.pbf"),
                 "18/20/14 PBF1 faces", 0);
    struct lyrics_vocal timed = lyrics_vocal_at(0, true), whole = lyrics_vocal_at(1, true);
    probe_expect(timed.timed && timed.begin == 1000 && timed.end == 3000 &&
                 !whole.timed && whole.begin == 1500 && whole.end == 3000,
                 "separate word timing and whole-line windows", 0);

    unsigned dots_frames = 0, gap_frames = 0, mixed_frames = 0, selected_frames = 0;
    unsigned overlap_samples = 0, moving_gap_frames = 0;
    int first_dot_center = -1;
    unsigned char opacity[780] = {0};
    lv.entry_frames = lv.track_frames = 0;
    lv_entry_cancel();
    lv.track_fade = false;
    lv.gap = false;
    lv.gap_row = lv.pose_gap = -1;
    lv.mode = (struct lyrics_mode){0};
    fu108_clock = 1000000;
    lv_motion(0, false, fu108_clock);

    for (unsigned f = 0; f < ARRAYLEN(opacity); ++f)
    {
        fu108_clock = 1000000 + ui_frame_clock(f) + 1;
        lv.elapsed = f * 1000 / 60;
        lv_follow(lv.elapsed, false);
        probe_draw();
        if (f == 141)
        {
            unsigned lit[PROBE_VOICES], ink[PROBE_VOICES];
            memcpy(lit, probe_peak, sizeof(lit));
            memcpy(ink, probe_ink, sizeof(ink));
            probe_dump(dir, "main-bg-duet");
            lv.elapsed = 1650;
            probe_draw();
            probe_expect(probe_ink[1] < ink[1] && probe_ink[3] == ink[3],
                         "timed background sweeps while untimed background lights in full", f);
            probe_dump(dir, "background-word-sweep");
            lv.elapsed = 0;
            probe_draw();
            for (unsigned i = 0; i < PROBE_VOICES; ++i)
            {
                probe_expect(lit[i] > probe_peak[i], "main, background and duet light together", i);
                fprintf(stderr, "FU108 vocal %u lit=%u dim=%u\n", i, lit[i], probe_peak[i]);
            }
            probe_dump(dir, "vocals-dim");
            lv.elapsed = f * 1000 / 60;
            overlap_samples++;
        }
        if (!lv.gap)
            continue;
        gap_frames++;
        unsigned dots = 0;
        int xmin = LCD_WIDTH, ymin = LCD_HEIGHT, ymax = -1;
        for (unsigned i = 0; i < LV_PIXELS; ++i)
        {
            probe_expect(probe_pixels[i] != (PROBE_DOT | PROBE_TEXT), "dots overlap lyric ink", f);
            if (!(probe_pixels[i] & PROBE_DOT))
                continue;
            dots++;
            xmin = MIN(xmin, (int)(i % LCD_WIDTH));
            ymin = MIN(ymin, (int)(i / LCD_WIDTH));
            ymax = MAX(ymax, (int)(i / LCD_WIDTH));
        }
        if (lv.motion_frame < lv.motion_frames)
            moving_gap_frames++;
        if (!dots)
            continue;
        dots_frames++;
        probe_expect(ymax - ymin + 1 == 8, "native dot height", f);
        opacity[f] = lv.old_gap_opacity - 1;
        if (lv.motion_frame == lv.motion_frames)
        {
            if (first_dot_center < 0)
            {
                first_dot_center = ymin;
                for (unsigned i = 0; i < LV_PIXELS; ++i)
                    stable_dots[i] = probe_pixels[i] & PROBE_DOT;
            }
            for (unsigned i = 0; i < LV_PIXELS; ++i)
                probe_expect((probe_pixels[i] & PROBE_DOT) == stable_dots[i],
                             "breathing changes opacity only", f);
        }
        if (lv.old_gap_opacity == 256 && f < 520 && f > 450)
        {
            probe_dump(dir, "gap-dots");
            fprintf(stderr, "FU108 dot crop %d %d\n", xmin, ymin);
        }
    }
    unsigned repeated = 0;
    for (unsigned f = 0; f + LV_GAP_PERIOD < ARRAYLEN(opacity); ++f)
        if (opacity[f] && opacity[f + LV_GAP_PERIOD] && opacity[f] == opacity[f + LV_GAP_PERIOD])
            repeated++;
    probe_expect(gap_frames > 240 && dots_frames > 240 && repeated > 90 && moving_gap_frames > 0,
                 "complete gap and repeated breath", repeated);
    probe_expect(!lv.gap && lv.mode.focus == 2, "gap closes at the next lyric", lv.mode.focus);
    probe_expect(overlap_samples == 1, "concurrent TTML sample", overlap_samples);

    /* Compare the world baseline of every visible row before and after each
     * wheel detent. Shapes may cross-fade. The baseline is immutable. */
    int world[PROBE_ROWS];
    for (unsigned i = 0; i < PROBE_ROWS; ++i)
        world[i] = INT_MIN;
    lv.gap = false;
    lv.gap_row = lv.pose_gap = -1;
    lv.mode = (struct lyrics_mode){.focus = 3};
    for (unsigned f = 0; f <= 100; ++f)
    {
        fu108_clock = 14000000 + ui_frame_clock(f) + 1;
        if (f == 0 || f == 8 || f == 28 || f == 57)
        {
            lv.mode.input = current_tick - HZ;
            lv.mode.direction = 0;
            lv_browse(f < 28 ? 1 : -1, fu108_clock);
        }
        probe_draw();
        int shift = lv.wheel_offset - lv_distance(3, lv.mode.focus, -1, true, -1);
        for (unsigned i = 0; i < PROBE_ROWS; ++i)
        {
            if (probe_baseline[i] == INT_MIN)
                continue;
            int baseline = probe_baseline[i] - shift;
            if (world[i] != INT_MIN)
                probe_expect(baseline == world[i], "manual baseline moves only with offset", f);
            world[i] = baseline;
        }
        unsigned selected = probe_sizes[lv.highlighted];
        mixed_frames += selected == 3;
        selected_frames += selected == 2;
        if (f == 21 || f == 77)
            probe_dump(dir, f == 21 ? "manual-crossfade" : "manual-selected");
    }
    probe_expect(mixed_frames && selected_frames, "manual cross-fade and 20 px face", mixed_frames);
    fprintf(stderr, "FU108 PASS gap=%u dots=%u repeat=%u moving=%u mixed=%u selected20=%u\n",
            gap_frames, dots_frames, repeated, moving_gap_frames, mixed_frames, selected_frames);
    fu108_clock = 0;
}
#endif
