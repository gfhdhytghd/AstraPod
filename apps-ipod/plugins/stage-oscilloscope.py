#!/usr/bin/env python3
"""Stage the ipod6g Oscilloscope with the upstream stereo plotters."""
from hashlib import sha256
from pathlib import Path
import sys


source_path, output_path = map(Path, sys.argv[1:])
upstream = source_path.read_bytes()
# Copy only reviewed plotters. Upstream callback and USB channel management
# stay out of the generated source.
if sha256(upstream).hexdigest() != (
    "0afe39bb971c09ce3730baf4730b6ed32632322f144a530454ff1ee1dc2abfd3"
):
    raise SystemExit(f"{source_path}: review the Oscilloscope staging")

source = upstream.decode()


def section(start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


header = source[:source.index('#include "plugin.h"')]
delay_table = section("/*\n *                   / 99*", "#define GRAPH_PEAKS_DEFAULT")
plotters = section("/* Draw cursor bar for horizontal views */", "/** Waveform View **/")
plotters = plotters.replace("LCD_HEIGHT", "GRAPH_HEIGHT")
for axis, length in (("h", "0, 0, 1, GRAPH_HEIGHT"), ("v", "0, 0, LCD_WIDTH, 1")):
    block = f"""    if (paused)
    {{
        one_frame_paused = false;
        osd_lcd_update_prepare();
        anim_draw_cursor_{axis}(0);
        osd_lcd_update_rect({length});
        last_pos = -1;
        return cur_tick + HZ/5;
    }}

"""
    if plotters.count(block) != 1:
        raise SystemExit(f"{source_path}: unexpected pause block ({axis})")
    plotters = plotters.replace(block, "")
plotters = plotters.replace("    one_frame_paused = false;\n\n    osd_lcd_update_prepare();\n\n", "")
plotters = plotters.replace("osd_lcd_update_rect", "update_plot_rect")
plotters = plotters.replace("osd_lcd_update()", "rb->lcd_update()")
plotters = plotters.replace("xlcd_scroll_left(shift)", "scroll_plot(shift, false)")
plotters = plotters.replace("xlcd_scroll_up(shift)", "scroll_plot(shift, true)")

peaks = """    static struct pcm_peaks peaks;
    rb->mixer_channel_calculate_peaks(channel,
                                      &peaks);"""
if plotters.count(peaks) != 1:
    raise SystemExit(f"{source_path}: unexpected peak reader")
plotters = plotters.replace(peaks, """    struct pcm_peaks peaks = {0};
    if (playback == AUDIO_STATUS_PLAY)
        rb->mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);""")

wave_plotters = (
    section("static void anim_waveform_plot_filled_h", "static long anim_waveform_horizontal")
    + section("static void anim_waveform_plot_filled_v", "static long anim_waveform_vertical")
)
scale = section("#define WAVEFORM_SCALE_PCM", "static void waveform_buffer_set_threshold")

output_path.write_text(header + r'''#include "plugin.h"
#include "lib/configfile.h"
#include "lib/helper.h"
#include "lib/pluginlib_exit.h"

#if LCD_DEPTH != 16 || LCD_STRIDEFORMAT != HORIZONTAL_STRIDE
#error Oscilloscope requires a row-major RGB565 framebuffer
#endif

#define BACKG_COLOR LCD_BLACK
#define GRAPH_COLOR LCD_RGBPACK(128, 255, 0)
#define CURSOR_COLOR LCD_RGBPACK(255, 0, 0)
#define HEADER_HEIGHT 24
#define GRAPH_HEIGHT (LCD_HEIGHT - HEADER_HEIGHT)
#define MIN_SPEED 1
#define MAX_SPEED 100
#define CFGFILE_VERSION 1

enum { DRAW_FILLED, DRAW_LINE, DRAW_PIXEL, MAX_DRAW };
enum { ADV_SCROLL, ADV_WRAP, MAX_ADV };
enum { OSC_HORIZ, OSC_VERT, MAX_OSC };
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
enum { GRAPH_PEAKS, GRAPH_WAVEFORM, MAX_GRAPH };
#define WINDOW_MIN_MS 2
#define WINDOW_MAX_MS 10
#endif

''' + delay_table + r'''static struct osc_config
{
    int speed;
    int draw;
    int advance;
    int orientation;
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
    int mode;
    int window_ms;
#endif
} osc_disk = {
    .speed = 68, .draw = DRAW_FILLED, .advance = ADV_SCROLL,
    .orientation = OSC_HORIZ,
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
    .mode = GRAPH_PEAKS, .window_ms = WINDOW_MAX_MS,
#endif
};

static struct osc_config osc;
static const char cfg_filename[] = "oscilloscope.cfg";
static char *draw_str[] = {"filled", "line", "pixel"};
static char *advance_str[] = {"scroll", "wrap"};
static char *orientation_str[] = {"horizontal", "vertical"};
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
static char *mode_str[] = {"peaks", "waveform"};
#endif
static struct configdata disk_config[] =
{
    {TYPE_INT, MIN_SPEED, MAX_SPEED, {.int_p = &osc_disk.speed}, "speed", NULL},
    {TYPE_ENUM, 0, MAX_DRAW, {.int_p = &osc_disk.draw}, "draw", draw_str},
    {TYPE_ENUM, 0, MAX_ADV, {.int_p = &osc_disk.advance}, "advance", advance_str},
    {TYPE_ENUM, 0, MAX_OSC, {.int_p = &osc_disk.orientation}, "orientation",
     orientation_str},
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
    {TYPE_ENUM, 0, MAX_GRAPH, {.int_p = &osc_disk.mode}, "graphmode", mode_str},
    {TYPE_INT, WINDOW_MIN_MS, WINDOW_MAX_MS, {.int_p = &osc_disk.window_ms},
     "window ms", NULL},
#endif
};

static struct viewport plot;
static int playback;
static long osc_delay;
static long osc_delay_error;

static void update_plot_rect(int x, int y, int width, int height)
{
    rb->lcd_update_rect(x, y + HEADER_HEIGHT,
                        MIN(width, LCD_WIDTH - x),
                        MIN(height, GRAPH_HEIGHT - y));
}

/* Scroll only the graph. The title lives in the rows above it. */
static void scroll_plot(int count, bool vertical)
{
    fb_data *pixels = plot.buffer->get_address_fn(0, HEADER_HEIGHT);
    size_t stride = plot.buffer->stride;
    if (vertical)
        rb->memmove(pixels, pixels + count * stride,
                    (GRAPH_HEIGHT - count) * stride * sizeof(*pixels));
    else
        for (int y = 0; y < GRAPH_HEIGHT; y++, pixels += stride)
            rb->memmove(pixels, pixels + count,
                        (LCD_WIDTH - count) * sizeof(*pixels));
}

''' + plotters + r'''#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
''' + scale + wave_plotters + r'''static long anim_waveform(void)
{
    static int16_t samples[2 * PCM_WAVEFORM_FRAMES];
    static void (* const draw[2][MAX_DRAW])(int, int, int, int, int, int) =
    {
        {anim_waveform_plot_filled_h, anim_waveform_plot_lines_h,
         anim_waveform_plot_pixel_h},
        {anim_waveform_plot_filled_v, anim_waveform_plot_lines_v,
         anim_waveform_plot_pixel_v},
    };

    long now = *rb->current_tick;
    size_t frames = playback == AUDIO_STATUS_PLAY
                  ? rb->waveform_read(samples, osc.window_ms) : 0;
    bool vertical = osc.orientation == OSC_VERT;
    int length = vertical ? GRAPH_HEIGHT : LCD_WIDTH;
    int depth = vertical ? LCD_WIDTH : GRAPH_HEIGHT;

    rb->lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, GRAPH_HEIGHT);
    rb->lcd_set_drawmode(DRMODE_SOLID);

    for (int channel = 0; channel < 2; channel++)
    {
        int center = (2 * channel + 1) * depth / 4;
        if (!frames)
        {
            if (vertical)
                rb->lcd_vline(center, 0, length - 1);
            else
                rb->lcd_hline(0, length - 1, center);
            continue;
        }

        int prev_min = center, prev_max = center;
        size_t first = 0;
        for (int pos = 0; pos < length; pos++)
        {
            size_t next = (pos + 1) * frames / length;
            int low = samples[2 * first + channel];
            int high = low;
            for (size_t i = first + 1; i < next; i++)
            {
                int sample = samples[2 * i + channel];
                low = MIN(low, sample);
                high = MAX(high, sample);
            }
            int min = WAVEFORM_SCALE_PCM(depth / 4 - 1, low);
            int max = WAVEFORM_SCALE_PCM(depth / 4 - 1, high);
            min = vertical ? center + min : center - min;
            max = vertical ? center + max : center - max;
            if (pos == 0)
                prev_min = min, prev_max = max;
            draw[vertical][osc.draw](pos, MAX(0, pos - 1), min, max,
                                     prev_min, prev_max);
            prev_min = min;
            prev_max = max;
            first = next;
        }
    }
    rb->lcd_update();
    return now + HZ/20;
}
#endif

static void reset_plot(void)
{
    last_left = last_right = last_pos = 0;
    last_tick = *rb->current_tick;
    last_delay = 1;
    osc_delay = osc_delay_tbl[osc.speed - 1];
    osc_delay_error = 0;

    rb->lcd_set_viewport(NULL);
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_background(BACKG_COLOR);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_setfont(FONT_SYSFIXED);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_clear_display();
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
    rb->lcd_putsxyf(4, 3, "Oscilloscope - %s",
                   osc.mode == GRAPH_WAVEFORM ? "Waveform" : "Peaks");
#else
    rb->lcd_putsxy(4, 3, "Oscilloscope - Peak only");
#endif
    const char *state = "Playing";
    if (!(playback & AUDIO_STATUS_PLAY))
        state = "Stopped";
    else if (playback & AUDIO_STATUS_PAUSE)
        state = "Paused";
    rb->lcd_putsxyf(4, 13, "%s   Select: Options   Menu: Exit", state);
    rb->lcd_update();

    plot = **rb->screens[SCREEN_MAIN]->current_viewport;
    plot.y = HEADER_HEIGHT;
    plot.height = GRAPH_HEIGHT;
    plot.fg_pattern = GRAPH_COLOR;
    rb->lcd_set_viewport(&plot);
}

static bool show_options(void)
{
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
    MENUITEM_STRINGLIST(menu, "Oscilloscope", NULL,
                        "Draw mode", "Advance", "Orientation", "Speed",
                        "Volume", "Graph mode", "Quit");
    MENUITEM_STRINGLIST(wave_menu, "Oscilloscope", NULL,
                        "Draw mode", "Time span", "Orientation", "Volume",
                        "Graph mode", "Quit");
    static const struct opt_items modes[] = {
        {"Peaks", -1}, {"Waveform", -1}
    };
    static const int wave_actions[] = {0, 7, 2, 4, 5, 6};
#else
    MENUITEM_STRINGLIST(menu, "Oscilloscope - Peak only", NULL,
                        "Draw mode", "Advance", "Orientation", "Speed",
                        "Volume", "Quit");
#endif
    static const struct opt_items draws[] = {
        {"Filled", -1}, {"Line", -1}, {"Pixel", -1}
    };
    static const struct opt_items advances[] = {
        {"Scroll", -1}, {"Wrap", -1}
    };
    static const struct opt_items orientations[] = {
        {"Horizontal", -1}, {"Vertical", -1}
    };
    int selected = 0;

    while (!rb->plugin_usb_taken())
    {
        int action;
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
        if (osc.mode == GRAPH_WAVEFORM)
        {
            action = rb->do_menu(&wave_menu, &selected, NULL, false);
            if (action >= 0 && action < (int)ARRAYLEN(wave_actions))
                action = wave_actions[action];
        }
        else
#endif
            action = rb->do_menu(&menu, &selected, NULL, false);

        switch (action)
        {
        case 0:
            rb->set_option("Draw mode", &osc.draw, RB_INT, draws,
                           ARRAYLEN(draws), NULL);
            break;
        case 1:
            rb->set_option("Advance", &osc.advance, RB_INT, advances,
                           ARRAYLEN(advances), NULL);
            break;
        case 2:
            rb->set_option("Orientation", &osc.orientation, RB_INT, orientations,
                           ARRAYLEN(orientations), NULL);
            break;
        case 3:
            rb->set_int("Speed", "", UNIT_INT, &osc.speed, NULL,
                        1, MIN_SPEED, MAX_SPEED, NULL);
            break;
        case 4:
            rb->option_screen(rb->find_setting(&rb->global_status->volume),
                              NULL, false, "Volume");
            break;
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
        case 5:
            rb->set_option("Graph mode", &osc.mode, RB_INT, modes,
                           ARRAYLEN(modes), NULL);
            selected = osc.mode == GRAPH_WAVEFORM ? 4 : 5;
            break;
        case 7:
            rb->set_int("Time span", "ms", UNIT_MS, &osc.window_ms, NULL,
                        1, WINDOW_MIN_MS, WINDOW_MAX_MS, NULL);
            break;
        case 6:
#else
        case 5:
#endif
            return true;
        default:
            return false;
        }
    }

    return false;
}

static void osc_cleanup(void)
{
    backlight_use_settings();
    if (rb->memcmp(&osc, &osc_disk, sizeof(osc)))
    {
        osc_disk = osc;
        configfile_save(cfg_filename, disk_config, ARRAYLEN(disk_config),
                        CFGFILE_VERSION);
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    configfile_load(cfg_filename, disk_config, ARRAYLEN(disk_config), 0);
    osc = osc_disk;
    atexit(osc_cleanup);
    backlight_ignore_timeout();
    playback = rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
    reset_plot();

    while (true)
    {
        int status = rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
        if (playback != status)
        {
            playback = status;
            reset_plot();
        }

        long next;
#ifdef ROCK_OSCILLOSCOPE_WAVEFORM
        if (osc.mode == GRAPH_WAVEFORM)
            next = anim_waveform();
        else
#endif
            next = osc.orientation == OSC_HORIZ ? anim_peaks_horizontal()
                                                : anim_peaks_vertical();
        long delay = next - *rb->current_tick;
        if (delay <= 0)
            rb->yield();
        int button = rb->button_get_w_tmo(MIN(MAX(delay, 0), HZ/5));
        switch (button)
        {
        case BUTTON_MENU:
            return PLUGIN_OK;
        case BUTTON_SELECT | BUTTON_REL:
        {
            bool quit = show_options();
            if (rb->plugin_usb_taken())
                return PLUGIN_USB_CONNECTED;
            if (quit)
                return PLUGIN_OK;
            playback = rb->audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE);
            reset_plot();
            break;
        }
        default:
            /* Let the firmware handle USB consent and shutdown. */
            exit_on_usb(button);
            break;
        }
    }
}
''')
