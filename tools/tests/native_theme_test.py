#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise native theme validation and RGB565 palette resolution under ASan/UBSan."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / "albumart"))
from host import compile_sources, link, run


objects = compile_sources(["apps-ipod/ui/native_theme.c", "apps-ipod/draw/color.c"])
probe = link("native-theme-test", r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "lcd.h"
#include "apps-ipod/ui/native_theme.h"
#include "apps-ipod/draw/color.h"

#define VERSION "podbox-theme=1\n"

void panicf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}

static unsigned packed(uint32_t rgb)
{
    return LCD_RGBPACK((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

static void reject(struct ui_theme *theme, const char *buffer, size_t length)
{
    unsigned char previous[sizeof(*theme)];
    memcpy(previous, theme, sizeof(previous));
    assert(!ui_theme_parse(theme, buffer, length));
    assert(!memcmp(previous, theme, sizeof(previous)));
}

static void check_builtins(void)
{
    struct ui_theme dark, light;
    ui_theme_builtin(&dark, false);
    ui_theme_builtin(&light, true);
    assert(dark.color[UI_COLOR_BACKGROUND] == 0);
    assert(dark.color[UI_COLOR_FOREGROUND] == 0xffffff);
    assert(dark.color[UI_COLOR_MUTED] == 0xbdbdbd);
    assert(dark.color[UI_COLOR_STATUS_BG] == 0xcecece);
    assert(dark.color[UI_COLOR_STATUS_FG] == 0);
    assert(dark.metric[UI_METRIC_ROW_HEIGHT] == 30);
    assert(dark.metric[UI_METRIC_LIST_X] == 10);
    assert(dark.metric[UI_METRIC_LIST_Y] == 46);
    assert(dark.metric[UI_METRIC_LIST_BOTTOM] == 10);
    assert(dark.metric[UI_METRIC_STATUS_HEIGHT] == 35);
    assert(dark.metric[UI_METRIC_STATUS_TEXT_Y] == 8);
    assert(dark.metric[UI_METRIC_STATUS_ICON_Y] == 5);
    assert(!strcmp(dark.font[UI_FONT_LIST], "18-noto-sans"));
    assert(!strcmp(dark.font[UI_FONT_TITLE], "18-noto-sans-bold"));
    assert(!strcmp(dark.font[UI_FONT_ICON], "24x24-icons"));
    assert(!strcmp(dark.font[UI_FONT_WPS_TIME], "12-noto-sans"));
    assert(dark.background[UI_BG_LIST] == UI_BG_BLURRED_COVER);
    assert(dark.background[UI_BG_WPS] == UI_BG_COVER_SCRIM);
    assert(dark.background[UI_BG_HANDOFF] == UI_BG_COVER_SCRIM);
    assert(dark.background[UI_BG_STATUS] == UI_BG_COVER_TINT);
    assert(dark.color[UI_COLOR_HANDOFF_MUTED] == 0x999999);
    assert(dark.metric[UI_METRIC_HANDOFF_COVER_SIZE] == 96);
    assert(dark.metric[UI_METRIC_SPLASH_PROGRESS_GAP] == 4);
    assert(light.background[UI_BG_LIST] == UI_BG_SOLID);
    assert(light.background[UI_BG_WPS] == UI_BG_SOLID);
    assert(light.background[UI_BG_STATUS] == UI_BG_COVER_TINT);
    assert(light.color[UI_COLOR_BACKGROUND] != dark.color[UI_COLOR_BACKGROUND]);
    assert(light.color[UI_COLOR_FOREGROUND] != dark.color[UI_COLOR_FOREGROUND]);
    assert(dark.motion_scale == 100 && light.motion_scale == 100);
    for (int i = 0; i < UI_COLOR_COUNT; i++)
        assert(light.dynamic[i].source == UI_PALETTE_NONE);
}

static void check_parser(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, true);

    static const char overrides[] =
        "  # 中文主题\r\n\r\n podbox-theme = 1 \r\n"
        "color.background = #aBc123\r\n"
        "color.foreground=012345\n"
        "palette.background = none\n"
        "palette.muted = vivid, 85, 100\n"
        "font.title = Local_20-Bold\n"
        "font.handoff_caption = Local_12\n"
        "metric.row_height = 40\n"
        "metric.splash_progress_radius=3\n"
        "background.wps=blurred_cover\n"
        "background.handoff=solid\n"
        "background.list=solid\n"
        "motion_scale=0";
    assert(ui_theme_parse(&theme, overrides, sizeof(overrides) - 1));
    assert(theme.color[UI_COLOR_BACKGROUND] == 0xabc123);
    assert(theme.color[UI_COLOR_FOREGROUND] == 0x012345);
    assert(theme.color[UI_COLOR_STATUS_BG] == 0xcecece);
    assert(theme.dynamic[UI_COLOR_BACKGROUND].source == UI_PALETTE_NONE);
    assert(theme.dynamic[UI_COLOR_MUTED].source == UI_PALETTE_VIVID);
    assert(theme.dynamic[UI_COLOR_MUTED].min_tone == 85);
    assert(theme.dynamic[UI_COLOR_MUTED].max_tone == 100);
    assert(!strcmp(theme.font[UI_FONT_TITLE], "Local_20-Bold"));
    assert(!strcmp(theme.font[UI_FONT_HANDOFF_CAPTION], "Local_12"));
    assert(theme.metric[UI_METRIC_ROW_HEIGHT] == 40);
    assert(theme.metric[UI_METRIC_SPLASH_PROGRESS_RADIUS] == 3);
    assert(theme.background[UI_BG_LIST] == UI_BG_SOLID);
    assert(theme.background[UI_BG_WPS] == UI_BG_BLURRED_COVER);
    assert(theme.background[UI_BG_HANDOFF] == UI_BG_SOLID);
    assert(theme.background[UI_BG_STATUS] == UI_BG_SOLID);
    assert(theme.motion_scale == 0);

    static const char tint[] = VERSION "background.status=cover_tint";
    assert(ui_theme_parse(&theme, tint, sizeof(tint) - 1));
    assert(theme.background[UI_BG_STATUS] == UI_BG_COVER_TINT);
    static const char solid[] = VERSION "background.status=solid";
    assert(ui_theme_parse(&theme, solid, sizeof(solid) - 1));
    assert(theme.background[UI_BG_STATUS] == UI_BG_SOLID);

    static const char *const invalid[] = {
        "", "# comment only\n", "color.foreground=abcdef", "podbox-theme=2\n",
        VERSION "podbox-theme=1", VERSION "fontsize", VERSION "font=abc",
        VERSION "color.unknown=000000", VERSION "color.background=fefef",
        VERSION "background.status=blurred_cover",
        VERSION "background.list=cover_tint",
        VERSION "background.status=solid\nbackground.status=cover_tint",
        VERSION "color.background=G00000", VERSION "color.background=ff00ff#comment",
        VERSION "color.background=ff00ff\ncolor.background=ff00ff",
        VERSION "palette.accent=vivid", VERSION "palette.accent=unseen,0,100",
        VERSION "palette.accent=dominant,90,80", VERSION "palette.accent=vivid,0,101",
        VERSION "palette.accent=vivid,-1,30", VERSION "palette.accent=accent,10,20,30",
        VERSION "font.list=../outside", VERSION "font.list=font.fnt",
        VERSION "font.icon=12345678901234567890123456789012", VERSION "font.title=",
        VERSION "metric.row_height=50px", VERSION "metric.row_height=99999999999999999999",
        VERSION "metric.row_height=16", VERSION "metric.status_height=46\nmetric.list_y=45",
        VERSION "metric.status_icon_y=20", VERSION "metric.list_y=120\nmetric.list_bottom=80\nmetric.row_height=50",
        VERSION "metric.list_selection_alpha=16", VERSION "metric.wps_meter_width=0",
        VERSION "metric.wps_meter_x=300", VERSION "metric.wps_meter_y=225",
        VERSION "metric.modal_edge_margin=48\nmetric.modal_padding=48\nmetric.modal_button_pad_x=32",
        VERSION "metric.handoff_margin=2", VERSION "metric.handoff_cover_size=160\nmetric.handoff_button_height=70",
        VERSION "metric.splash_progress_radius=65",
        VERSION "background.wps=0", VERSION "background.wps=jpeg",
        VERSION "background.home=solid", VERSION "motion_scale=-1",
        VERSION "motion_scale=301", VERSION "motion_scale=1.5",
        VERSION "motion_scale=50\nmotion_scale=100",
        VERSION "color.accent=123456\nfont.title=invalid/font",
        VERSION "# embedded\rnewline\n", VERSION "\vcolor.background=000000",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        reject(&theme, invalid[i], strlen(invalid[i]));

    static const char with_nul[] = VERSION "# Embedded \0 NUL.";
    reject(&theme, with_nul, sizeof(with_nul) - 1);
    reject(&theme, NULL, 1);

    char bounded[UI_THEME_FILE_MAX + 1];
    memset(bounded, 'x', sizeof(bounded));
    memcpy(bounded, VERSION "#", sizeof(VERSION "#") - 1);
    reject(&theme, bounded, sizeof(bounded));
    assert(ui_theme_parse(&theme, bounded, UI_THEME_FILE_MAX));
    assert(theme.motion_scale == 100);
    assert(theme.color[UI_COLOR_FOREGROUND] == 0xffffff);

    const char *positive = VERSION "motion_scale=300\nfont.list=1234567890123456789012345678901";
    assert(ui_theme_parse(&theme, positive, strlen(positive)));
    assert(theme.motion_scale == 300);
    assert(strlen(theme.font[UI_FONT_LIST]) == UI_THEME_FONT_NAME_MAX - 1);
}

static void check_palette(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, false);
    assert(ui_theme_color(&theme, UI_COLOR_MUTED, NULL) == 0xbdbdbd);
    struct ui_album_palette palette = {0};
    assert(ui_theme_color(&theme, UI_COLOR_SELECTION, &palette) == 0xffffff);

    palette.dominant = 0x70c040;
    palette.accent = 0xd8deae;
    palette.vivid = 0x2360aa;
    palette.has_dominant = palette.has_accent = palette.has_vivid = true;
    assert(packed(ui_theme_color(&theme, UI_COLOR_BACKGROUND, &palette)) ==
           color_tone_bound(packed(palette.dominant), 15, -1));
    assert(packed(ui_theme_color(&theme, UI_COLOR_MUTED, &palette)) ==
           color_tone_bound(packed(palette.vivid), 85, 1));
    assert(ui_theme_color(&theme, UI_COLOR_STATUS_BG, &palette) == 0xcecece);

    palette.has_vivid = false;
    assert(ui_theme_color(&theme, UI_COLOR_SELECTION, &palette) == palette.accent);

    static const char rules[] = VERSION
        "palette.accent=accent,0,100\n"
        "palette.dim=dominant,49,51\n"
        "palette.selection=vivid,0,30\n"
        "color.accent=ffffff\n";
    assert(ui_theme_parse(&theme, rules, sizeof(rules) - 1));
    palette.accent = 0;
    assert(ui_theme_color(&theme, UI_COLOR_ACCENT, &palette) == 0);
    assert(ui_theme_color(&theme, UI_COLOR_SELECTION, &palette) == 0);
    unsigned bounded = color_tone_bound(packed(palette.dominant), 49, 1);
    bounded = color_tone_bound(bounded, 51, -1);
    assert(packed(ui_theme_color(&theme, UI_COLOR_DIM, &palette)) == bounded);
    palette.has_accent = false;
    assert(ui_theme_color(&theme, UI_COLOR_ACCENT, &palette) == 0xffffff);
}

/* Classic and Plastic draw with stock's bitmaps. Their selection bar is
 * stock's rows, whatever the theme file sets:
 * exact at their own height, and with the edge lines kept when a row is
 * shorter. */
static void check_plastic(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, UI_THEME_CLASSIC);
    assert(theme.style[UI_STYLE_ART] == UI_ART_PLASTIC);
    ui_theme_builtin(&theme, UI_THEME_PLASTIC);
    assert(theme.style[UI_STYLE_ART] == UI_ART_PLASTIC);
    assert(!strcmp(theme.font[UI_FONT_WPS_TIME], "14-helvetica-bold"));
    const struct ui_theme_gradient *bar = &theme.gradient[UI_GRADIENT_SELECTION];
    assert(ui_theme_gradient_at(bar, 0, 25, 0) == 0x96c5da);
    assert(ui_theme_gradient_at(bar, 12, 25, 0) == 0x0484da);
    assert(ui_theme_gradient_at(bar, 24, 25, 0) == 0x004386);
    assert(ui_theme_gradient_at(bar, 1, 24, 0) == 0x09a4e1);
    assert(ui_theme_gradient_at(bar, 22, 24, 0) == 0x0065d3);
    assert(ui_theme_gradient_at(bar, 23, 24, 0) == 0x004386);

    static const char file[] =
        "podbox-theme=1\ngradient.selection=000000,ffffff\nstyle.art=plastic\n";
    assert(ui_theme_parse(&theme, file, sizeof(file) - 1));
    assert(ui_theme_gradient_at(&theme.gradient[UI_GRADIENT_SELECTION], 24, 25,
                                0) == 0x004386);
}

/* Only Classic and its Plastic split their menus, in the pane's own fonts. */
static void check_menu_pane(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, UI_THEME_SCRIM);
    assert(theme.style[UI_STYLE_MENU_PANE] == UI_MENU_PANE_NONE);
    ui_theme_builtin(&theme, UI_THEME_LIGHT);
    assert(theme.style[UI_STYLE_MENU_PANE] == UI_MENU_PANE_NONE);
    ui_theme_builtin(&theme, UI_THEME_CLASSIC);
    assert(theme.style[UI_STYLE_MENU_PANE] == UI_MENU_PANE_SPLIT);
    assert(!strcmp(theme.font[UI_FONT_PANE_TITLE], "17-helvetica-bold-pane"));
    assert(!strcmp(theme.font[UI_FONT_PANE_TEXT], "13-helvetica-bold"));
    ui_theme_builtin(&theme, UI_THEME_PLASTIC);
    assert(theme.style[UI_STYLE_MENU_PANE] == UI_MENU_PANE_SPLIT);

    static const char file[] = VERSION "style.menu_pane=split\n"
        "font.pane_title=Local_20-Bold\nfont.pane_text=Local_12\n";
    assert(ui_theme_parse(&theme, file, sizeof(file) - 1));
    assert(theme.style[UI_STYLE_MENU_PANE] == UI_MENU_PANE_SPLIT);
    assert(!strcmp(theme.font[UI_FONT_PANE_TITLE], "Local_20-Bold"));
    assert(!strcmp(theme.font[UI_FONT_PANE_TEXT], "Local_12"));
}

/* Classic and Plastic share out the list's spare height among the rows and
 * show a setting's value in the smaller stock face; Scrim and Light do not. */
static void check_list_rows(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, UI_THEME_SCRIM);
    assert(theme.style[UI_STYLE_LIST_ROWS] == UI_LIST_ROWS_PITCH);
    ui_theme_builtin(&theme, UI_THEME_LIGHT);
    assert(theme.style[UI_STYLE_LIST_ROWS] == UI_LIST_ROWS_PITCH);
    ui_theme_builtin(&theme, UI_THEME_CLASSIC);
    assert(theme.style[UI_STYLE_LIST_ROWS] == UI_LIST_ROWS_FILL);
    assert(!strcmp(theme.font[UI_FONT_LIST_VALUE], "13-helvetica-bold"));
    ui_theme_builtin(&theme, UI_THEME_PLASTIC);
    assert(theme.style[UI_STYLE_LIST_ROWS] == UI_LIST_ROWS_FILL);

    static const char file[] = VERSION "style.list_rows=fill\n"
        "font.list_value=Local_12\n";
    assert(ui_theme_parse(&theme, file, sizeof(file) - 1));
    assert(theme.style[UI_STYLE_LIST_ROWS] == UI_LIST_ROWS_FILL);
    assert(!strcmp(theme.font[UI_FONT_LIST_VALUE], "Local_12"));
}

/* Classic and Plastic fade the screens that are not lists; Scrim and Light
 * move them. */
static void check_screen_entry(void)
{
    struct ui_theme theme;
    ui_theme_builtin(&theme, UI_THEME_SCRIM);
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_MOVE);
    ui_theme_builtin(&theme, UI_THEME_LIGHT);
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_MOVE);
    ui_theme_builtin(&theme, UI_THEME_CLASSIC);
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_FADE);
    ui_theme_builtin(&theme, UI_THEME_PLASTIC);
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_FADE);

    static const char fade[] = VERSION "style.screen_entry=fade\n";
    ui_theme_builtin(&theme, UI_THEME_SCRIM);
    assert(ui_theme_parse(&theme, fade, sizeof(fade) - 1));
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_FADE);
    static const char move[] = VERSION "style.screen_entry=move\n";
    assert(ui_theme_parse(&theme, move, sizeof(move) - 1));
    assert(theme.style[UI_STYLE_SCREEN_ENTRY] == UI_SCREEN_ENTRY_MOVE);
    static const char bad[] = VERSION "style.screen_entry=slide\n";
    reject(&theme, bad, sizeof(bad) - 1);
}

int main(void)
{
    check_builtins();
    check_parser();
    check_palette();
    check_plastic();
    check_menu_pane();
    check_list_rows();
    check_screen_entry();
    puts("PASS native theme defaults, overrides, atomic validation, palette bounds, menu pane, list rows and screen entry");
    return 0;
}
''', objects)
print(run([probe], "run-native-theme").stdout.strip())
