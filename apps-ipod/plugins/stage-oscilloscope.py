#!/usr/bin/env python3
"""Stage the ipod6g Oscilloscope with the upstream peak plotters only."""
from hashlib import sha256
from pathlib import Path
import sys


source_path, output_path = map(Path, sys.argv[1:])
upstream = source_path.read_bytes()
# Only copy ranges from the reviewed source. In particular, neither the
# waveform paths nor the USB channel switch (which clears a hook) enter C.
if sha256(upstream).hexdigest() != (
    "0afe39bb971c09ce3730baf4730b6ed32632322f144a530454ff1ee1dc2abfd3"
):
    raise SystemExit(f"{source_path}: review the Oscilloscope peak-only staging")

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

output_path.write_text(header + r'''#include "plugin.h"
#include "lib/configfile.h"
#include "lib/helper.h"
#include "lib/pluginlib_exit.h"

#if LCD_DEPTH != 16 || LCD_STRIDEFORMAT != HORIZONTAL_STRIDE
#error Oscilloscope peak display requires a row-major RGB565 framebuffer
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

''' + delay_table + r'''static struct osc_config
{
    int speed;
    int draw;
    int advance;
    int orientation;
} osc_disk = {68, DRAW_FILLED, ADV_SCROLL, OSC_HORIZ};

static struct osc_config osc;
static const char cfg_filename[] = "oscilloscope.cfg";
static char *draw_str[] = {"filled", "line", "pixel"};
static char *advance_str[] = {"scroll", "wrap"};
static char *orientation_str[] = {"horizontal", "vertical"};
static struct configdata disk_config[] =
{
    {TYPE_INT, MIN_SPEED, MAX_SPEED, {.int_p = &osc_disk.speed}, "speed", NULL},
    {TYPE_ENUM, 0, MAX_DRAW, {.int_p = &osc_disk.draw}, "draw", draw_str},
    {TYPE_ENUM, 0, MAX_ADV, {.int_p = &osc_disk.advance}, "advance", advance_str},
    {TYPE_ENUM, 0, MAX_OSC, {.int_p = &osc_disk.orientation}, "orientation",
     orientation_str},
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

''' + plotters + r'''static void reset_plot(void)
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
    rb->lcd_putsxy(4, 3, "Oscilloscope - Peak only");
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
    MENUITEM_STRINGLIST(menu, "Oscilloscope - Peak only", NULL,
                        "Draw mode", "Advance", "Orientation", "Speed",
                        "Volume", "Quit");
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
        switch (rb->do_menu(&menu, &selected, NULL, false))
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
        case 5:
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

        long next = osc.orientation == OSC_HORIZ ? anim_peaks_horizontal()
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
