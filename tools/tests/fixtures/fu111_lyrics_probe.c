/* Native glyph comparison on a disposable SDL build. */
static const char *fu111_case;
static unsigned fu111_phase, fu111_checked[4], fu111_bg;
static bool fu111_row;
static fb_data fu111_reference[LV_PIXELS];
static uint8_t fu111_expected[LV_PIXELS];

static void *fu111_address(int x, int y)
{
    return fu111_reference + y * LCD_WIDTH + x;
}

static void fu111_begin(int i, bool bg, int font, const char *text,
                        int y, int alpha, int align, int inset)
{
    if (!fu111_case)
        return;
    if (strcmp(fu111_case, "ink"))
    {
        if (!lv.gap || bg || (i != lv.gap_row && i != lv.gap_row - 1))
            return;
        int amount = i < lv.gap_row ? global_settings.lyric_prev_opacity :
                                     global_settings.lyric_next_opacity;
        if (font != lv.font || alpha != (amount * 256 + 50) / 100)
        {
            fprintf(stderr, "FU111 gap style line=%d gap=%d alpha=%d at %ld ms\n",
                    i, lv.gap_row, alpha, lv.elapsed);
            abort();
        }
    }
    else if (!bg && font != lv.large_font)
        return;

    /* Compare entire glyphs that fit inside the lyric viewport. This lets us
     * exercise the scrolling edge without confusing its intentional crop
     * with a crop at the font or row boundary. */
    const int ref_y = 32;
    const int native_y = y + inset;
    if (native_y < LV_BODY_TOP + 6 || native_y + font_get(font)->height + 8 >= LV_BODY_BOTTOM)
        return;

    struct frame_buffer_t fb = {.fb_ptr = fu111_reference, .get_address_fn = fu111_address,
        .stride = LCD_WIDTH, .elems = LV_PIXELS};
    struct viewport vp = lv.vp;
    vp.x = vp.y = 0;
    vp.width = LV_STRIP_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.buffer = &fb;
    struct viewport *saved = lcd_set_viewport(&vp);
    memset(fu111_reference, 0, sizeof(fu111_reference));
    lcd_setfont(font);
    lcd_set_foreground(LCD_WHITE);
    lcd_set_drawmode(DRMODE_FG);
    lcd_putsxy(0, ref_y, text);
    lcd_set_viewport(saved);

    int width = MIN(LV_STRIP_WIDTH, font_getstringsize((const unsigned char *)text, NULL, NULL, font));
    int first = width, end = 0;
    for (int row = 0; row < LCD_HEIGHT; row++)
        for (int x = 0; x < width; x++)
            if (fu111_reference[row * LCD_WIDTH + x])
            {
                first = MIN(first, x);
                end = MAX(end, x + 1);
            }
    if (align != 0) first = 0;
    if (align != 2) end = width;
    int left = lv_text_x(end - first, align);
    memset(fu111_expected, 0, sizeof(fu111_expected));
    memset(fu111_actual, 0, sizeof(fu111_actual));
    for (int row = LV_BODY_TOP + 1; row < LV_BODY_BOTTOM; row++)
    {
        int ref = row - native_y + ref_y;
        if (ref < 0 || ref >= LCD_HEIGHT) continue;
        for (int x = first; x < end; x++)
        {
            unsigned green = (fu111_reference[ref * LCD_WIDTH + x] >> 5) & 63;
            fu111_expected[row * LCD_WIDTH + left + x - first] = green + (green >> 5);
        }
    }
    fu111_row = true;
    if (bg)
        fu111_bg++;
}

static void fu111_end(void)
{
    if (!fu111_row)
        return;
    fu111_row = false;
    unsigned missing = 0, extra = 0, reference = 0;
    for (unsigned k = 0; k < LV_PIXELS; ++k)
    {
        unsigned expected = fu111_expected[k], actual = fu111_actual[k];
        reference += expected != 0;
        missing += expected && expected != actual;
        extra += !expected && actual;
    }
    if (!reference || missing || extra)
    {
        fprintf(stderr, "FU111 1:1 mismatch phase=%u ms=%ld reference=%u missing=%u extra=%u\n",
                fu111_phase, lv.elapsed, reference, missing, extra);
        abort();
    }
    fu111_checked[fu111_phase]++;
}

static void fu111_frame(unsigned frame)
{
    fu108_clock = 2000000 + ui_frame_clock(frame) + 1;
    probe_draw();
}

static void fu111_short_gaps(const char *dir)
{
    unsigned switch_frame = 0, long_dots = 0, boundary_dots = 0;
    for (unsigned f = 0; f < 573; f++)
    {
        fu108_clock = 2000000 + ui_frame_clock(f) + 1;
        lv.elapsed = f * 1000 / 60;
        lv_follow(lv.elapsed, false);
        probe_draw();
        bool dots = false;
        for (unsigned i = 0; i < LV_PIXELS && !dots; i++)
            dots = (probe_pixels[i] & PROBE_DOT) != 0;
        if (f < 150)
        {
            probe_expect(!dots && !lv.gap, "1.5 second gap never draws dots", f);
            if (!switch_frame && lv.mode.focus == 1)
                switch_frame = f;
        }
        else if (f >= 210 && f < 360)
        {
            probe_expect(lv.gap && lv.mode.focus == 1, "2.5 second gap owns focus", f);
            long_dots += dots;
            if (f == 270) probe_dump(dir, "long-gap");
        }
        else if (f >= 450 && f < 570)
        {
            probe_expect(lv.gap && lv.mode.focus == 2, "2 second boundary gap", f);
            boundary_dots += dots;
        }
    }
    probe_expect(abs((int)switch_frame - 105) <= 1, "switch at gap midpoint", switch_frame);
    probe_expect(long_dots > 100 && boundary_dots > 80, "dots visible in long gaps", long_dots);
    fprintf(stderr, "FU111 PASS midpoint_f=%u short_dots=0 long_dots=%u boundary_dots=%u\n",
            switch_frame, long_dots, boundary_dots);
}

static void fu111_probe(const char *dir, const char *test)
{
    probe_expect(lyrics_ttml() && lv.masks, "TTML and native strips", 0);
    if (!strncmp(test, "photo", 5))
    {
        bool baseline = !strcmp(test, "photo-base");
        const unsigned char *sample = (const unsigned char *)"囉個驚妳謊揮卻暈遺忘臉";
        unsigned present = 0;
        int faces[] = {lv.font, lv.large_font, lv.small_font};
        for (unsigned i = 0; i < ARRAYLEN(faces); i++)
        {
            font_lock(faces[i], true);
            struct font *font = font_get(faces[i]);
            for (const unsigned char *p = sample; *p;)
            {
                ucschar_t cp;
                p = utf8decode(p, &cp);
                bool have = font_has_glyph(font, cp);
                if (!baseline || cp == 0x500b || cp == 0x5fd8)
                    probe_expect(have, "photo glyph native lookup", cp);
                present += have;
            }
            font_lock(faces[i], false);
        }
        fprintf(stderr, "FU111 PASS lookup_hits=%u lookup_requests=33\n", present);
        return;
    }
    lv_entry_cancel();
    lv.track_fade = false;
    lv.mode = (struct lyrics_mode){0};
    fu108_clock = 2000000;
    lv_follow(0, true);
    lv.track_fade = false;
    if (!strcmp(test, "short"))
    {
        fu111_short_gaps(dir);
        fu108_clock = 0;
        return;
    }
    fu111_case = test;

    if (strcmp(test, "ink"))
    {
        unsigned count = 0;
        for (unsigned f = 0; f <= 740; f++)
        {
            fu108_clock = 2000000 + ui_frame_clock(f) + 1;
            lv.elapsed = f * 1000 / 60;
            lv_follow(lv.elapsed, false);
            fu111_frame(f);
            if (lv.gap)
                count++;
            if (f == 410 && lv.gap)
                probe_dump(dir, "gap");
        }
        probe_expect(count > 200 && fu111_checked[0] >= count, "whole gap sampled", count);
        fprintf(stderr, "FU111 PASS gap_frames=%u native_rows=%u\n", count, fu111_checked[0]);
    }
    else
    {
        for (unsigned f = 0; f < 145; ++f)
        {
            fu108_clock = 2000000 + ui_frame_clock(f) + 1;
            lv.elapsed = f * 1000 / 60;
            lv_follow(lv.elapsed, false);
            fu111_frame(f);
            if (f == 94) probe_dump(dir, "crossfade");
        }
        probe_dump(dir, "duet");
        fu111_phase = 1;
        lv_browse(1, fu108_clock);
        for (unsigned f = 145; f < 182; ++f)
            fu111_frame(f);
        probe_dump(dir, "manual");

        fu111_phase = 2;
        probe_expect(lv_select_seek(fu108_clock) && lv.seek_pending, "manual pick sends a seek", 0);
        for (unsigned f = 182; f < 205; ++f)
            fu111_frame(f);
        probe_dump(dir, "seek");

        fu111_phase = 3;
        /* Changing TTML clock on a settled pose must repaint the same
         * pixels as a complete redraw, including background and descenders. */
        fu111_case = NULL;
        lv.elapsed += 90;
        probe_expect(lv_draw(false, LV_UPDATE), "incremental word redraw", 0);
        static fb_data damage[LV_PIXELS];
        memcpy(damage, lv_frame(), LV_FRAME);
        lv_draw(true, LV_UPDATE);
        probe_expect(!memcmp(damage, lv_frame(), LV_FRAME), "incremental equals full RGB565", 0);
        for (unsigned i = 0; i < fu111_phase; ++i)
            probe_expect(fu111_checked[i] > 10, "native reference exercised", i);
        fprintf(stderr, "FU111 PASS auto=%u manual=%u seek=%u background=%u damage_diff=0\n",
                fu111_checked[0], fu111_checked[1], fu111_checked[2], fu111_bg);
    }
    fu111_case = NULL;
    fu108_clock = 0;
}
