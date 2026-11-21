#!/usr/bin/env python3
"""Stage the pinned Pitch Detector with AstraPod foreground capture."""
import hashlib
from pathlib import Path
import sys


source, output = map(Path, sys.argv[1:])
data = source.read_bytes()
upstream_sha = "ee24d1b9be230ee970641488f93efd52cadfe6fa67ba2b020ae4bc7f52d46299"
if hashlib.sha256(data).hexdigest() != upstream_sha:
    sys.exit("Pitch Detector upstream source changed; review the capture adapter")
text = data.decode()


def replace(old, new):
    global text
    if text.count(old) != 1:
        sys.exit(f"Pitch Detector staging expected one match for {old!r}")
    text = text.replace(old, new)


def section(start, end, new):
    first = text.index(start)
    last = text.index(end, first)
    replace(text[first:last], new)


section("/* I introduce some divisors", "#define fp_freq2period", """\
#define fp_period2freq(x) ((fixed)(((int64_t)sample_rate << (2 * FIXED_PRECISION)) / (x)))
""")
replace("#define BUFFER_SIZE 4096\n#define SAMPLE_SIZE 4096",
        "#define BUFFER_SIZE ROCK_PITCH_SAMPLES")
section("/* Change to AUDIO_SRC_LINEIN", "/* How many decimal places", "")
section("static int audio_head", "/* Notes within one (reference) scale */", """#if !defined(SIMULATOR) || defined(ROCK_PITCH_ALGORITHM_TEST)
static int16_t samples[BUFFER_SIZE];
static fixed yin_buffer[YIN_BUFFER_SIZE];
#endif

""")
replace("static unsigned int sample_rate;", "static const int sample_rate = ROCK_PITCH_RATE;")
replace('"/.pitch_detector_settings"', '"/.astrapod_pitch_detector"')
replace("    unsigned record_gain;\n", "")
replace("        .record_gain = rb->global_settings->rec_mic_gain,\n", "")
section("    if(fd < 0){ /* file doesn't exist */", "\n/*---------------------------------------------------------------------*/", """    tuner_settings_reset();
    if (fd < 0)
        return;

    struct tuner_settings saved;
    bool complete = rb->ffilesize(fd) == sizeof(saved) &&
                    rb->read(fd, &saved, sizeof(saved)) == sizeof(saved);
    rb->close(fd);
    /* Validate the persisted values before they become indexes or bounds.
     * Read booleans as bytes to reject invalid representations. */
    if (complete && saved.volume_threshold >= 5 && saved.volume_threshold <= 95 &&
        saved.sample_size >= SAMPLE_SIZE_MIN && saved.sample_size <= BUFFER_SIZE &&
        saved.sample_size % 4 == 0 &&
        saved.lowest_freq >= sample_rate / (BUFFER_SIZE / 4) &&
        saved.lowest_freq <= sample_rate / (SAMPLE_SIZE_MIN / 4) &&
        (unsigned)saved.yin_threshold < 14 && (unsigned)saved.freq_A < NUM_FREQ_A &&
        (unsigned)saved.key_transposition < 12 &&
        *(unsigned char *)&saved.use_sharps <= 1 &&
        *(unsigned char *)&saved.display_hz <= 1)
    {
        settings = saved;
        hdd_settings = saved;
    }
}
""")
replace('    backlight_use_settings();\n#ifdef HAVE_SCHEDULER_BOOSTCTRL\n    rb->cancel_cpu_boost();\n#endif\n',
        '    backlight_use_settings();\n')
replace('                        "Listening Volume",\n', '')
section("            case 2:", "            case 3:", "")
for number in range(3, 11):
    replace(f"            case {number}:", f"            case {number - 1}:")
for field in ("volume_threshold", "lowest_freq", "yin_threshold"):
    replace(f"    unsigned {field};", f"    int {field};")
replace("            case 0:\n            default:", """            case MENU_ATTACHED_USB:
                return true;
            case 0:
            default:""")
replace("rb->lcd_set_foreground(LCD_RGBPACK(255,255,255)); /* Color screens */",
        "rb->lcd_set_foreground(front_color);")
replace("    rb->lcd_update();\n}\n\n#ifndef SIMULATOR", """#ifdef SIMULATOR
    rb->lcd_set_foreground(front_color);
    rb->lcd_putsxy(2, 2, "Preview - no audio capture");
#endif
    rb->lcd_update();
}

#if !defined(SIMULATOR) || defined(ROCK_PITCH_ALGORITHM_TEST)""")
replace("#ifndef SIMULATOR\nstatic const fixed yin_threshold_table",
        "#if !defined(SIMULATOR) || defined(ROCK_PITCH_ALGORITHM_TEST)\nstatic const fixed yin_threshold_table")
section("/* Find the index of the minimum element", "static uint32_t ICODE_ATTR buffer_magnitude", """\
/* Scale the sum after accumulating it to preserve small differences. */
static uint32_t difference(const int16_t *input, unsigned count, unsigned lag)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        int64_t delta = input[2 * i] - input[2 * (i + lag)];
        sum += delta * delta;
    }
    return sum >> FIXED_PRECISION;
}

static fixed pitchyin(const int16_t *input, fixed *yin)
{
    unsigned count = settings.sample_size / 4;
    uint64_t total = 0;
    yin[0] = int2fixed(1);

    for (unsigned lag = 1; lag < count; ++lag)
    {
        uint32_t energy = difference(input, count, lag);
        total += energy;
        yin[lag] = total ? (fixed)(((uint64_t)energy * lag << FIXED_PRECISION) / total)
                         : int2fixed(1);

        /* The shortest period considered also bounds the fixed-point Hz. */
        if (lag < 5 || yin[lag - 1] >= yin_threshold_table[settings.yin_threshold] ||
            yin[lag - 1] >= yin[lag] || yin[lag - 1] >= yin[lag - 2])
            continue;

        /* Fit the minimum of the raw difference curve. Normalization
         * selects the first credible trough but skews its interpolation. */
        int32_t left = difference(input, count, lag - 2);
        int32_t center = difference(input, count, lag - 1);
        int32_t right = energy;
        fixed period = int2fixed(lag - 1);
        if (center < left && center < right)
            period += ((int64_t)(left - right) * int2fixed(1)) /
                      (2 * (left - 2 * center + right));
        return period;
    }
    return FP_ZERO;
}

""")
section("/* Stop the recording when the buffer is full */", "    /* GUI */", """static fixed detect_frequency(void)
{
    /* Threshold is in tenths of a percent of signed PCM full scale. */
    int amplitude = settings.volume_threshold * INT16_MAX / 1000;
    if (buffer_magnitude(samples) <= (uint32_t)sqr(amplitude))
        return FP_ZERO;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    fixed period = pitchyin(samples, yin_buffer);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
    return fp_gt(period, FP_ZERO) ? fp_period2freq(period) : FP_ZERO;
}
#endif

static enum plugin_status record_and_get_pitch(void)
{
    enum plugin_status result = PLUGIN_OK;
#ifdef SIMULATOR
    const fixed initial_frequency = int2fixed(445);
#else
    const fixed initial_frequency = FP_ZERO;
#endif
    backlight_ignore_timeout();

    if (!rb->pitch_start())
    {
        result = PLUGIN_ERROR;
        goto out;
    }
    display_frequency(initial_frequency);

    while (true)
    {
        int action = pluginlib_getaction(1, plugin_contexts, PLA_ARRAY_COUNT);
        if (IS_SYSEVENT(action) &&
            rb->default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = PLUGIN_USB_CONNECTED;
            break;
        }

        if (action == PLA_EXIT)
            break;
        if (action == PLA_CANCEL || action == PLA_UP)
        {
            rb->pitch_pause();
            if (main_menu())
            {
                if (rb->plugin_usb_taken())
                    result = PLUGIN_USB_CONNECTED;
                break;
            }
            if (!rb->pitch_start())
                break; /* The session can be revoked while in the menu. */

            display_frequency(initial_frequency);
        }

#ifdef SIMULATOR
        enum rock_pitch_result capture = rb->pitch_read(NULL);
#else
        enum rock_pitch_result capture = rb->pitch_read(&samples);
        if (capture == ROCK_PITCH_READY)
            display_frequency(detect_frequency());
#endif
        if (capture == ROCK_PITCH_STOPPED)
            break;
        if (capture == ROCK_PITCH_FAILED)
        {
            result = PLUGIN_ERROR;
            break;
        }
    }

out:
    rb->pitch_pause();
    backlight_use_settings();
    return result;
}

static void init_everything(void)
{
    load_settings();

""")
replace("\n    rb->talk_disable(false);\n", "\n")
replace("    record_and_get_pitch();\n    save_settings();\n\n    return PLUGIN_OK;", """    enum plugin_status result = record_and_get_pitch();
    if (result == PLUGIN_OK)
        save_settings();
    return result;""")
replace('"Volume Threshold", "%", UNIT_INT,', '"Signal Threshold", "0.1%", UNIT_INT,')
replace('                        "Volume Threshold",', '                        "Signal Threshold",')

output.write_text(text)
