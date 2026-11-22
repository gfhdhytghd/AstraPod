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
#ifndef SIMULATOR
static uint32_t captured_frames;
static uint32_t analyzed_frames;
static unsigned int input_peak;
#endif

""")
replace("static unsigned int sample_rate;", "static const int sample_rate = ROCK_PITCH_RATE;")
replace("#define VOLUME_THRESHOLD (50)", """\
#define VOLUME_THRESHOLD 8
#define SIGNAL_THRESHOLD_MIN 5
#define SIGNAL_THRESHOLD_MAX 95""")
replace('"/.pitch_detector_settings"', '"/.astrapod_pitch_detector"')
replace("    unsigned record_gain;\n", "")
replace("        .record_gain = rb->global_settings->rec_mic_gain,\n", "")
section("    if(fd < 0){ /* file doesn't exist */", "\n/*---------------------------------------------------------------------*/", """    tuner_settings_reset();
    hdd_settings = settings;
    if (fd < 0)
        return;

    struct tuner_settings saved;
    bool complete = rb->ffilesize(fd) == sizeof(saved) &&
                    rb->read(fd, &saved, sizeof(saved)) == sizeof(saved);
    rb->close(fd);
    /* Validate the persisted values before they become indexes or bounds.
     * Read booleans as bytes to reject invalid representations. */
    if (complete && saved.volume_threshold >= SIGNAL_THRESHOLD_MIN &&
        saved.volume_threshold <= SIGNAL_THRESHOLD_MAX &&
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
section("    int fd = rb->creat(SETTINGS_FILENAME, 0666);",
        "\n/*=================================================================*/", """\
    const char *temp = SETTINGS_FILENAME ".tmp";
    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd >= 0)
    {
        bool complete = rb->write(fd, &settings, sizeof(settings)) ==
                        sizeof(settings) && rb->file_sync(fd) == 0;
        if (rb->close(fd) == 0 && complete &&
            rb->rename(temp, SETTINGS_FILENAME) == 0)
            return;
        rb->remove(temp);
    }
    rb->splash(HZ, "Unable to save tuner settings");
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
    rb->lcd_putsxy(2, HZ_Y + (settings.display_hz ? font_h : 0),
                  "Preview - no audio capture");
#else
    if (fp_round(freq) == 0)
    {
        rb->lcd_set_foreground(front_color);
        rb->lcd_putsxy(2, HZ_Y, "No pitch. Check mic or line input.");
    }
    rb->lcd_set_foreground(front_color);
    rb->lcd_putsxyf(2, LCD_HEIGHT - font_h, "In %lu  Skip %lu  Peak %u",
                   (unsigned long)captured_frames,
                   (unsigned long)(captured_frames - analyzed_frames),
                   input_peak);
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
section("static uint32_t ICODE_ATTR buffer_magnitude", "    /* GUI */", """\
/* 3 kHz Butterworth low-pass at 22.05 kHz, with Q14 coefficients.
 * Initialize from this block so a skipped DMA block cannot carry filter state. */
static void filter_input(int16_t *input)
{
    int32_t x1 = input[0], x2 = x1;
    int32_t y1 = x1, y2 = x1;
    for (unsigned i = 0; i < (unsigned)settings.sample_size; i += 2)
    {
        int32_t x = input[i];
        int32_t y = (1836 * (x + x2) + 3671 * x1 +
                     14025 * y1 - 4984 * y2 + 8192) >> 14;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        input[i] = y > INT16_MAX ? INT16_MAX : y < INT16_MIN ? INT16_MIN : y;
    }
}

static uint32_t buffer_magnitude(const int16_t *input)
{
    int32_t sum = 0;
    uint64_t squares = 0;
    const unsigned frames = settings.sample_size / 2;
    for (unsigned i = 0; i < frames; ++i)
    {
        int32_t sample = input[2 * i];
        sum += sample;
        squares += sample * sample;
    }
    /* AC mean square: remove the input's DC component from the level gate. */
    return (squares - (int64_t)sum * sum / frames) / frames;
}

static fixed detect_frequency(void)
{
    filter_input(samples);
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

static void display_capture_start(void)
{
#ifdef SIMULATOR
    display_frequency(int2fixed(445));
#else
    captured_frames = 0;
    analyzed_frames = 0;
    input_peak = 0;
    display_frequency(FP_ZERO);
#endif
}

static enum plugin_status record_and_get_pitch(void)
{
    enum plugin_status result = PLUGIN_OK;
    backlight_ignore_timeout();

    if (!rb->pitch_start())
    {
        rb->splash(HZ, "Tuner input unavailable");
        result = PLUGIN_ERROR;
        goto out;
    }
    display_capture_start();

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
            {
                if (rb->plugin_usb_taken())
                    result = PLUGIN_USB_CONNECTED;
                else
                {
                    rb->splash(HZ, "Tuner input unavailable");
                    result = PLUGIN_ERROR;
                }
                break;
            }

            display_capture_start();
        }

#ifdef SIMULATOR
        enum rock_pitch_result capture = rb->pitch_read(NULL, NULL);
#else
        enum rock_pitch_result capture = rb->pitch_read(&samples, &captured_frames);
        if (capture == ROCK_PITCH_READY)
        {
            unsigned int peak = 0;
            for (unsigned int i = 0; i < BUFFER_SIZE; i += 2)
                peak = MAX(peak, (unsigned int)abs(samples[i]));
            input_peak = peak;
            analyzed_frames += ROCK_PITCH_CAPTURE_FRAMES;
            display_frequency(detect_frequency());
        }
#endif
        if (capture == ROCK_PITCH_STOPPED)
            break;
        if (capture == ROCK_PITCH_NO_INPUT)
        {
            rb->splash(2 * HZ, "Tuner microphone unplugged");
            result = PLUGIN_ERROR;
            break;
        }
        if (capture == ROCK_PITCH_FAILED)
        {
            rb->splash(HZ, "Tuner capture failed");
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
replace('                               NULL, 5, 5, 95, NULL);',
        '                               NULL, 1, SIGNAL_THRESHOLD_MIN,\n'
        '                               SIGNAL_THRESHOLD_MAX, NULL);')

output.write_text(text)
