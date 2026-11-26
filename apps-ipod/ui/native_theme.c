/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "native_theme.h"

#include <string.h>

#include "config.h"
#include "lcd.h"
#include "draw/color.h"
#include "plastic_columns.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const struct {
    const char *key;
    uint32_t scrim, light;
    struct ui_theme_dynamic palette;
} colors[UI_COLOR_COUNT] = {
    [UI_COLOR_BACKGROUND]         = { "background",         0x000000, 0xf6f5f2, {UI_PALETTE_DOMINANT, 0, 15} },
    [UI_COLOR_FOREGROUND]         = { "foreground",         0xffffff, 0x181818, {0} },
    [UI_COLOR_MUTED]              = { "muted",              0xbdbdbd, 0x505050, {UI_PALETTE_VIVID, 85, 100} },
    [UI_COLOR_ACCENT]             = { "accent",             0xffffff, 0x12588e, {UI_PALETTE_VIVID, 60, 100} },
    [UI_COLOR_SELECTION]          = { "selection",          0xffffff, 0x12588e, {UI_PALETTE_VIVID, 60, 100} },
    [UI_COLOR_SELECTED_SECONDARY] = { "selected_secondary", 0xd6d6d6, 0x505050, {0} },
    [UI_COLOR_SELECTED_TEXT]      = { "selected_text",      0xffffff, 0x181818, {0} },
    [UI_COLOR_SEPARATOR]          = { "separator",          0x333333, 0xc7c5bf, {0} },
    [UI_COLOR_DIM]                = { "dim",                0x000000, 0x000000, {0} },
    [UI_COLOR_STATUS_BG]          = { "status_bg",          0xcecece, 0xdddcd7, {0} },
    [UI_COLOR_STATUS_FG]          = { "status_fg",          0x000000, 0x181818, {0} },
    [UI_COLOR_MODAL_BG]           = { "modal_bg",           0x101010, 0xffffff, {0} },
    [UI_COLOR_MODAL_FG]           = { "modal_fg",           0xffffff, 0x181818, {0} },
    [UI_COLOR_MODAL_BORDER]       = { "modal_border",       0x636563, 0x73706c, {0} },
    [UI_COLOR_MODAL_ACCENT]       = { "modal_accent",       0xd6d3d6, 0x12588e, {0} },
    [UI_COLOR_MODAL_SELECTED_TEXT] = { "modal_selected_text", 0x101010, 0xffffff, {0} },
    [UI_COLOR_MODAL_SHADOW]       = { "modal_shadow",       0x000000, 0x73706c, {0} },
    [UI_COLOR_WPS_BG]             = { "wps_bg",             0x000000, 0xf6f5f2, {0} },
    [UI_COLOR_WPS_FG]             = { "wps_fg",             0xffffff, 0x181818, {0} },
    [UI_COLOR_WPS_MUTED]          = { "wps_muted",          0x333333, 0x505050, {0} },
    [UI_COLOR_WPS_ACCENT]         = { "wps_accent",         0xffffff, 0x12588e, {UI_PALETTE_VIVID, 60, 100} },
    [UI_COLOR_WPS_METER]          = { "wps_meter",          0xffffff, 0x181818, {0} },
    [UI_COLOR_WPS_PANEL]          = { "wps_panel",          0x000000, 0xdddcd7, {UI_PALETTE_DOMINANT, 0, 30} },
    [UI_COLOR_LIST_TINT]          = { "list_tint",          0x000000, 0xdddcd7, {UI_PALETTE_DOMINANT, 0, 40} },
    [UI_COLOR_HANDOFF_BG]          = { "handoff_bg",         0x000000, 0xf6f5f2, {0} },
    [UI_COLOR_HANDOFF_FG]          = { "handoff_fg",         0xffffff, 0x181818, {0} },
    [UI_COLOR_HANDOFF_MUTED]       = { "handoff_muted",      0x999999, 0x505050, {0} },
    [UI_COLOR_HANDOFF_UNSELECTED_TEXT] = { "handoff_unselected_text", 0xcccccc, 0x181818, {0} },
    [UI_COLOR_HANDOFF_SELECTED_TEXT]   = { "handoff_selected_text",   0x000000, 0xffffff, {0} },
    [UI_COLOR_HANDOFF_COVER_PLACEHOLDER] = { "handoff_cover_placeholder", 0x181818, 0xdddcd7, {0} },
};

static const struct {
    const char *key, *name;
} fonts[UI_FONT_COUNT] = {
    [UI_FONT_LIST]      = { "list",      "18-noto-sans" },
    [UI_FONT_TITLE]     = { "title",     "18-noto-sans-bold" },
    [UI_FONT_BODY]      = { "body",      "18-noto-sans" },
    [UI_FONT_LYRICS]    = { "lyrics",    "18-noto-sans" },
    [UI_FONT_STATUS]    = { "status",    "18-noto-sans" },
    [UI_FONT_ICON]      = { "icon",      "24x24-icons" },
    [UI_FONT_WPS_TITLE] = { "wps_title", "26-noto-sans-medium" },
    [UI_FONT_WPS_META]  = { "wps_meta",  "12-noto-sans" },
    [UI_FONT_HANDOFF_CAPTION] = { "handoff_caption", "12-noto-sans" },
    [UI_FONT_HANDOFF_TITLE] = { "handoff_title", "26-noto-sans-medium" },
    [UI_FONT_LYRICS_FOCUS] = { "lyrics_focus", "20-noto-sans" },
    [UI_FONT_LYRICS_BACKGROUND] = { "lyrics_background", "14-noto-sans" },
    [UI_FONT_PANEL]     = { "panel",     "14-noto-sans" },
    [UI_FONT_STATUS_TITLE] = { "status_title", "18-noto-sans-bold" },
    [UI_FONT_PANE_TITLE] = { "pane_title", "18-noto-sans-bold" },
    [UI_FONT_PANE_TEXT] = { "pane_text", "14-noto-sans" },
};

static const struct {
    const char *key;
    uint16_t value, min, max;
} metrics[UI_METRIC_COUNT] = {
    [UI_METRIC_ROW_HEIGHT]                     = { "row_height",               30, 16, 80 },
    [UI_METRIC_LIST_X]                         = { "list_x",                   10,  0, 80 },
    [UI_METRIC_LIST_Y]                         = { "list_y",                   46,  0, 120 },
    [UI_METRIC_LIST_BOTTOM]                    = { "list_bottom",              10,  0, 80 },
    [UI_METRIC_LIST_ICON_SIZE]                 = { "list_icon_size",           24,  8, 64 },
    [UI_METRIC_LIST_ICON_MARGIN]               = { "list_icon_margin",          2,  0, 40 },
    [UI_METRIC_LIST_ICON_GAP]                  = { "list_icon_gap",            10,  0, 40 },
    [UI_METRIC_LIST_TEXT_PADDING]              = { "list_text_padding",       12,  0, 40 },
    [UI_METRIC_LIST_TEXT_HEIGHT]               = { "list_text_height",         20,  8, 64 },
    [UI_METRIC_LIST_RIGHT_PADDING]             = { "list_right_padding",       4,  0, 40 },
    [UI_METRIC_LIST_SELECTION_WIDTH]           = { "list_selection_width",      3,  0, 16 },
    [UI_METRIC_LIST_SELECTION_ALPHA]           = { "list_selection_alpha",     14,  0, 15 },
    [UI_METRIC_LIST_ACCESSORY_GAP]             = { "list_accessory_gap",        9,  0, 40 },
    [UI_METRIC_LIST_TINT_ALPHA]                = { "list_tint_alpha",          15,  0, 15 },
    [UI_METRIC_STATUS_HEIGHT]                  = { "status_height",           35, 12, 72 },
    [UI_METRIC_STATUS_PAD_LEFT]                = { "status_pad_left",          10,  0, 40 },
    [UI_METRIC_STATUS_PAD_RIGHT]               = { "status_pad_right",          7,  0, 40 },
    [UI_METRIC_STATUS_ICON_SIZE]               = { "status_icon_size",         24,  8, 48 },
    [UI_METRIC_STATUS_ICON_GAP]                = { "status_icon_gap",           2,  0, 32 },
    [UI_METRIC_STATUS_GROUP_GAP]               = { "status_group_gap",          9,  0, 32 },
    [UI_METRIC_STATUS_CLOCK_WIDTH]             = { "status_clock_width",      70, 12, 200 },
    [UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH]     = { "status_clock_numeric_width", 118, 12, 220 },
    [UI_METRIC_STATUS_LINK_GAP]                = { "status_link_gap",          10,  0, 40 },
    [UI_METRIC_STATUS_TEXT_Y]                  = { "status_text_y",             8,  0, 64 },
    [UI_METRIC_STATUS_TITLE_HEIGHT]            = { "status_title_height",     20,  8, 64 },
    [UI_METRIC_STATUS_ICON_Y]                  = { "status_icon_y",             5,  0, 64 },
    [UI_METRIC_MODAL_PADDING]                  = { "modal_padding",            10,  0, 48 },
    [UI_METRIC_MODAL_EDGE_MARGIN]              = { "modal_edge_margin",        12,  0, 48 },
    [UI_METRIC_MODAL_SHADOW_OFFSET]            = { "modal_shadow_offset",       4,  0, 16 },
    [UI_METRIC_MODAL_BORDER_WIDTH]             = { "modal_border_width",        2,  0,  8 },
    [UI_METRIC_MODAL_BUTTON_GAP]               = { "modal_button_gap",         18,  0, 48 },
    [UI_METRIC_MODAL_BUTTON_PAD_X]             = { "modal_button_pad_x",       12,  0, 32 },
    [UI_METRIC_MODAL_BUTTON_PAD_Y]             = { "modal_button_pad_y",        5,  0, 24 },
    [UI_METRIC_COVER_SIZE]                     = { "cover_size",             240, 16, 240 },
    [UI_METRIC_WPS_TRANSPORT_X]                = { "wps_transport_x",           2,  0, 319 },
    [UI_METRIC_WPS_TRANSPORT_Y]                = { "wps_transport_y",           2,  0, 239 },
    [UI_METRIC_WPS_TRANSPORT_WIDTH]            = { "wps_transport_width",      76,  1, 320 },
    [UI_METRIC_WPS_TRANSPORT_HEIGHT]           = { "wps_transport_height",     24,  1, 240 },
    [UI_METRIC_WPS_TITLE_X]                    = { "wps_title_x",               5,  0, 319 },
    [UI_METRIC_WPS_TITLE_Y]                    = { "wps_title_y",              85,  0, 239 },
    [UI_METRIC_WPS_TITLE_WIDTH]                = { "wps_title_width",         230,  1, 320 },
    [UI_METRIC_WPS_TITLE_HEIGHT]               = { "wps_title_height",        28,  1, 240 },
    [UI_METRIC_WPS_SUBTITLE_Y]                 = { "wps_subtitle_y",          113,  0, 239 },
    [UI_METRIC_WPS_SUBTITLE_HEIGHT]            = { "wps_subtitle_height",     20,  1, 240 },
    [UI_METRIC_WPS_PROGRESS_WIDTH]             = { "wps_progress_width",     240,  1, 320 },
    [UI_METRIC_WPS_PROGRESS_HEIGHT]            = { "wps_progress_height",      5,  1, 240 },
    [UI_METRIC_WPS_PROGRESS_Y]                 = { "wps_progress_y",         140,  0, 239 },
    [UI_METRIC_WPS_PROGRESS_ALPHA]             = { "wps_progress_alpha",       6,  0, 15 },
    [UI_METRIC_WPS_DECK_INDEX_X]               = { "wps_deck_index_x",          5,  0, 319 },
    [UI_METRIC_WPS_DECK_TIME_X]                = { "wps_deck_time_x",          45,  0, 319 },
    [UI_METRIC_WPS_DECK_Y]                     = { "wps_deck_y",              150,  0, 239 },
    [UI_METRIC_WPS_DECK_WIDTH]                 = { "wps_deck_width",          30,  1, 320 },
    [UI_METRIC_WPS_DECK_HEIGHT]                = { "wps_deck_height",         14,  1, 240 },
    [UI_METRIC_WPS_METER_X]                    = { "wps_meter_x",               0,  0, 319 },
    [UI_METRIC_WPS_METER_Y]                    = { "wps_meter_y",             200,  0, 239 },
    [UI_METRIC_WPS_METER_WIDTH]                = { "wps_meter_width",         80,  1, 320 },
    [UI_METRIC_WPS_METER_HEIGHT]               = { "wps_meter_height",        40,  1, 240 },
    [UI_METRIC_WPS_METER_PEAK_HEIGHT]          = { "wps_meter_peak_height",     2,  0, 40 },
    [UI_METRIC_WPS_PANEL_TOP]                 = { "wps_panel_top",           77,  0, 239 },
    [UI_METRIC_WPS_PANEL_ALPHA]               = { "wps_panel_alpha",         10,  0, 15 },
    [UI_METRIC_WPS_VOLUME_X]                  = { "wps_volume_x",             40,  0, 319 },
    [UI_METRIC_WPS_VOLUME_Y]                  = { "wps_volume_y",             15,  0, 239 },
    [UI_METRIC_WPS_VOLUME_WIDTH]              = { "wps_volume_width",        240,  1, 320 },
    [UI_METRIC_WPS_VOLUME_HEIGHT]             = { "wps_volume_height",         6,  1, 240 },
    [UI_METRIC_HANDOFF_MARGIN]                = { "handoff_margin",            16,  0, 48 },
    [UI_METRIC_HANDOFF_COVER_SIZE]            = { "handoff_cover_size",        96, 32, 160 },
    [UI_METRIC_HANDOFF_BUTTON_HEIGHT]        = { "handoff_button_height",      36, 12, 72 },
    [UI_METRIC_HANDOFF_BUTTON_GAP]           = { "handoff_button_gap",          8,  0, 48 },
    [UI_METRIC_HANDOFF_METADATA_GAP]         = { "handoff_metadata_gap",       12,  0, 32 },
    [UI_METRIC_HANDOFF_LINE_GAP]             = { "handoff_line_gap",            3,  0, 24 },
    [UI_METRIC_HANDOFF_PROGRESS_GAP]         = { "handoff_progress_gap",        5,  0, 24 },
    [UI_METRIC_HANDOFF_PROGRESS_HEIGHT]      = { "handoff_progress_height",     2,  1, 16 },
    [UI_METRIC_HANDOFF_SCRIM_ALPHA]          = { "handoff_scrim_alpha",          8,  0, 15 },
    [UI_METRIC_HANDOFF_OUTLINE_WIDTH]        = { "handoff_outline_width",       1,  0,  8 },
    [UI_METRIC_HANDOFF_COVER_BORDER_ALPHA]   = { "handoff_cover_border_alpha",  4,  0, 15 },
    [UI_METRIC_HANDOFF_BUTTON_BORDER_ALPHA]  = { "handoff_button_border_alpha", 5,  0, 15 },
    [UI_METRIC_HANDOFF_PROGRESS_ALPHA]       = { "handoff_progress_alpha",       4,  0, 15 },
    [UI_METRIC_HANDOFF_ERROR_OFFSET]         = { "handoff_error_offset",         4,  0, 32 },
    [UI_METRIC_SPLASH_PROGRESS_GAP]          = { "splash_progress_gap",          4,  0, 48 },
    [UI_METRIC_SPLASH_PROGRESS_RADIUS]       = { "splash_progress_radius",       2,  0, 64 },
    [UI_METRIC_LIST_ART_SIZE]                 = { "list_art_size",                44,  8, 72 },
    [UI_METRIC_LIST_ART_ROW_HEIGHT]           = { "list_art_row_height",          48, 16, 96 },
    [UI_METRIC_LIST_ART_TOP_GAP]              = { "list_art_top_gap",              5,  0, 20 },
    [UI_METRIC_LIST_ART_BOTTOM_GAP]           = { "list_art_bottom_gap",           3,  0, 20 },
    [UI_METRIC_LIST_HEADER_HEIGHT]            = { "list_header_height",          68,  0, 120 },
    [UI_METRIC_LIST_HEADER_BOTTOM_GAP]        = { "list_header_bottom_gap",        4,  0, 20 },
    [UI_METRIC_LIST_NOTES_HEIGHT]             = { "list_notes_height",           62,  0, 120 },
    [UI_METRIC_LIST_AUX_PADDING]              = { "list_aux_padding",             3,  0, 20 },
};

static const struct {
    const char *key;
    enum ui_bg_kind scrim;
} backgrounds[] = {
    [UI_BG_LIST]   = { "list",   UI_BG_BLURRED_COVER },
    [UI_BG_WPS]    = { "wps",    UI_BG_COVER_SCRIM },
    [UI_BG_LYRICS] = { "lyrics", UI_BG_BLURRED_COVER },
    [UI_BG_HANDOFF] = { "handoff", UI_BG_COVER_SCRIM },
};

static const char *const builtin_names[UI_THEME_BUILTIN_COUNT] = {
    [UI_THEME_SCRIM] = "scrim", [UI_THEME_LIGHT] = "light",
    [UI_THEME_CLASSIC] = "classic", [UI_THEME_PLASTIC] = "plastic",
};

static const struct {
    const char *key;
    const char *const *keywords;
} styles[UI_STYLE_COUNT] = {
    [UI_STYLE_SELECTION]    = { "selection",    (const char *const[]){ "marker", "bar", NULL } },
    [UI_STYLE_LIST_ICONS]   = { "list_icons",   (const char *const[]){ "auto", "off", NULL } },
    [UI_STYLE_ACCESSORY]    = { "accessory",    (const char *const[]){ "all", "selected", NULL } },
    [UI_STYLE_STATUS_CLOCK] = { "status_clock", (const char *const[]){ "on", "off", NULL } },
    [UI_STYLE_STATUS_TITLE] = { "status_title", (const char *const[]){ "left", "center", NULL } },
    [UI_STYLE_PAGE_MOTION]  = { "page_motion",  (const char *const[]){ "cascade", "slide", NULL } },
    [UI_STYLE_WPS_LAYOUT]   = { "wps_layout",   (const char *const[]){ "cover", "classic", NULL } },
    [UI_STYLE_SCROLLBAR]    = { "scrollbar",    (const char *const[]){ "plain", "classic", NULL } },
    [UI_STYLE_ART]          = { "art",          (const char *const[]){ "none", "plastic", NULL } },
    [UI_STYLE_MENU_PANE]    = { "menu_pane",    (const char *const[]){ "none", "split", NULL } },
    [UI_STYLE_LIST_SCROLL]  = { "list_scroll",  (const char *const[]){ "snap", "glide", NULL } },
};

static const char *const gradients[UI_GRADIENT_COUNT] = {
    [UI_GRADIENT_SELECTION] = "selection", [UI_GRADIENT_STATUS] = "status",
    [UI_GRADIENT_PROGRESS] = "progress",
    [UI_GRADIENT_PROGRESS_TRACK] = "progress_track",
    [UI_GRADIENT_SCROLLBAR] = "scrollbar",
};

static const char *const motions[UI_MOTION_COUNT] = {
    [UI_MOTION_PUSH] = "push", [UI_MOTION_POP] = "pop",
    [UI_MOTION_LIST] = "list", [UI_MOTION_SHEET_IN] = "sheet_in",
    [UI_MOTION_SHEET_OUT] = "sheet_out",
};

static const char *const easings[] = {
    [UI_EASE_CUBIC] = "cubic", [UI_EASE_QUAD] = "quad",
    [UI_EASE_QUINT] = "quint", [UI_EASE_SINE] = "sine",
    [UI_EASE_LINEAR] = "linear", NULL
};

/* Classic: the look of the stock iPod Classic firmware, on Light's base. */
static void classic(struct ui_theme *theme)
{
    static const struct { uint8_t role; uint32_t rgb; } color_set[] = {
        { UI_COLOR_BACKGROUND, 0xffffff }, { UI_COLOR_FOREGROUND, 0x000000 },
        { UI_COLOR_MUTED, 0x000000 }, { UI_COLOR_ACCENT, 0x3185d0 },
        { UI_COLOR_SELECTION, 0x0089de },
        { UI_COLOR_SELECTED_SECONDARY, 0xffffff },
        { UI_COLOR_SELECTED_TEXT, 0xffffff }, { UI_COLOR_SEPARATOR, 0xa5a6a5 },
        { UI_COLOR_STATUS_BG, 0xcecfce }, { UI_COLOR_STATUS_FG, 0x000000 },
        { UI_COLOR_LIST_TINT, 0xefefef },
        { UI_COLOR_MODAL_BG, 0xffffff }, { UI_COLOR_MODAL_FG, 0x000000 },
        { UI_COLOR_MODAL_BORDER, 0x5a595a }, { UI_COLOR_MODAL_ACCENT, 0x0089de },
        { UI_COLOR_MODAL_SELECTED_TEXT, 0xffffff },
        { UI_COLOR_WPS_BG, 0xffffff }, { UI_COLOR_WPS_FG, 0x000000 },
        { UI_COLOR_WPS_MUTED, 0x3d3d3d }, { UI_COLOR_WPS_ACCENT, 0x0071d6 },
        { UI_COLOR_WPS_METER, 0x0071d6 }, { UI_COLOR_WPS_PANEL, 0xffffff },
        { UI_COLOR_HANDOFF_BG, 0xffffff }, { UI_COLOR_HANDOFF_FG, 0x000000 },
        { UI_COLOR_HANDOFF_COVER_PLACEHOLDER, 0xdedfde },
    };
    static const struct { uint8_t role; uint16_t value; } metric_set[] = {
        { UI_METRIC_ROW_HEIGHT, 24 }, { UI_METRIC_LIST_X, 0 },
        { UI_METRIC_LIST_Y, 20 }, { UI_METRIC_LIST_BOTTOM, 0 },
        { UI_METRIC_LIST_TEXT_PADDING, 5 }, { UI_METRIC_LIST_RIGHT_PADDING, 0 },
        { UI_METRIC_LIST_ICON_MARGIN, 0 }, { UI_METRIC_LIST_ICON_GAP, 0 },
        { UI_METRIC_LIST_TEXT_HEIGHT, 20 }, { UI_METRIC_LIST_ACCESSORY_GAP, 2 },
        { UI_METRIC_LIST_SELECTION_WIDTH, 0 },
        { UI_METRIC_STATUS_HEIGHT, 20 }, { UI_METRIC_STATUS_PAD_LEFT, 6 },
        { UI_METRIC_STATUS_PAD_RIGHT, 7 }, { UI_METRIC_STATUS_TEXT_Y, 2 },
        { UI_METRIC_STATUS_TITLE_HEIGHT, 18 }, { UI_METRIC_STATUS_ICON_Y, 0 },
        { UI_METRIC_STATUS_ICON_SIZE, 20 },
        { UI_METRIC_MODAL_BORDER_WIDTH, 1 },
        { UI_METRIC_LIST_ART_SIZE, 55 }, { UI_METRIC_LIST_ART_ROW_HEIGHT, 55 },
        { UI_METRIC_LIST_ART_TOP_GAP, 0 }, { UI_METRIC_LIST_ART_BOTTOM_GAP, 0 },
    };
    for (size_t i = 0; i < ARRAY_COUNT(color_set); i++)
        theme->color[color_set[i].role] = color_set[i].rgb;
    for (size_t i = 0; i < ARRAY_COUNT(metric_set); i++)
        theme->metric[metric_set[i].role] = metric_set[i].value;
    strcpy(theme->font[UI_FONT_LIST], "16-helvetica-bold");
    strcpy(theme->font[UI_FONT_TITLE], "17-helvetica-bold");
    strcpy(theme->font[UI_FONT_BODY], "13-helvetica");
    strcpy(theme->font[UI_FONT_STATUS], "13-helvetica-bold");
    strcpy(theme->font[UI_FONT_STATUS_TITLE], "13-helvetica-bold");
    theme->gradient[UI_GRADIENT_SELECTION] = (struct ui_theme_gradient){
        .color = { 0x96c5da, 0x09a4e1, 0x0065d3, 0x004386 }, .stops = 4 };
    theme->gradient[UI_GRADIENT_STATUS] = (struct ui_theme_gradient){
        .color = { 0xfcfcfc, 0xf8f8f8, 0x9f9e9e, 0x5b5b5b }, .stops = 4 };
    strcpy(theme->font[UI_FONT_WPS_TITLE], "17-helvetica-bold");
    strcpy(theme->font[UI_FONT_WPS_META], "13-helvetica-bold");
    strcpy(theme->font[UI_FONT_PANE_TITLE], "17-helvetica-bold-pane");
    strcpy(theme->font[UI_FONT_PANE_TEXT], "13-helvetica-bold");
    theme->gradient[UI_GRADIENT_PROGRESS] = (struct ui_theme_gradient){
        .color = { 0x6483f0, 0x6789f2, 0x56aeff, 0x4e8fce }, .stops = 4 };
    theme->gradient[UI_GRADIENT_PROGRESS_TRACK] = (struct ui_theme_gradient){
        .color = { 0xdbdbdb, 0xfdfdfd, 0xe2e2e2, 0xbababa }, .stops = 4 };
    theme->background[UI_BG_STATUS] = UI_BG_SOLID;
    theme->style[UI_STYLE_SELECTION] = UI_SELECTION_BAR;
    theme->style[UI_STYLE_LIST_ICONS] = UI_LIST_ICONS_OFF;
    theme->style[UI_STYLE_ACCESSORY] = UI_ACCESSORY_SELECTED;
    theme->style[UI_STYLE_STATUS_CLOCK] = UI_STATUS_CLOCK_OFF;
    theme->style[UI_STYLE_PAGE_MOTION] = UI_PAGE_MOTION_SLIDE;
    /* The stock slides last 270 ms, 18 refreshes of the 67 Hz panel, with
     * quadratic ease-out; its list bar and rows move a whole row at once. */
    theme->motion[UI_MOTION_PUSH] = 18;
    theme->motion[UI_MOTION_POP] = 18;
    theme->motion[UI_MOTION_LIST] = 0;
    theme->easing = UI_EASE_QUAD;
    theme->style[UI_STYLE_WPS_LAYOUT] = UI_WPS_LAYOUT_CLASSIC;
    theme->style[UI_STYLE_SCROLLBAR] = UI_SCROLLBAR_CLASSIC;
    theme->style[UI_STYLE_MENU_PANE] = UI_MENU_PANE_SPLIT;
}

/* Apple's own bars take over the gradients they replace. */
static void plastic_columns(struct ui_theme *theme)
{
    static const struct {
        uint8_t role, rows;
        const uint32_t *column;
    } column_set[] = {
        { UI_GRADIENT_STATUS, ARRAY_COUNT(plastic_status), plastic_status },
        { UI_GRADIENT_SELECTION, ARRAY_COUNT(plastic_selection),
          plastic_selection },
        { UI_GRADIENT_PROGRESS, ARRAY_COUNT(plastic_progress),
          plastic_progress },
        { UI_GRADIENT_PROGRESS_TRACK, ARRAY_COUNT(plastic_progress_track),
          plastic_progress_track },
    };
    for (size_t i = 0; i < ARRAY_COUNT(column_set); i++)
    {
        struct ui_theme_gradient *gradient =
            &theme->gradient[column_set[i].role];
        const uint32_t *column = column_set[i].column;
        int last = column_set[i].rows - 1;
        *gradient = (struct ui_theme_gradient){
            .color = { column[0], column[1], column[last - 1], column[last] },
            .stops = 4, .nrows = column_set[i].rows, .rows = column };
    }
}

/* Plastic: Classic drawn with Apple's own bitmaps. */
static void plastic(struct ui_theme *theme)
{
    theme->style[UI_STYLE_ART] = UI_ART_PLASTIC;
    theme->metric[UI_METRIC_STATUS_PAD_RIGHT] = 5;
    theme->metric[UI_METRIC_STATUS_ICON_GAP] = 6;
    plastic_columns(theme);
}

void ui_theme_builtin(struct ui_theme *theme, enum ui_theme_builtin_id id)
{
    bool light = id != UI_THEME_SCRIM;
    memset(theme, 0, sizeof(*theme));
    for (size_t i = 0; i < ARRAY_COUNT(colors); i++)
    {
        theme->color[i] = light ? colors[i].light : colors[i].scrim;
        if (!light)
            theme->dynamic[i] = colors[i].palette;
    }
    for (size_t i = 0; i < ARRAY_COUNT(fonts); i++)
        strcpy(theme->font[i], fonts[i].name);
    for (size_t i = 0; i < ARRAY_COUNT(metrics); i++)
        theme->metric[i] = metrics[i].value;
    for (size_t i = 0; i < ARRAY_COUNT(backgrounds); i++)
        theme->background[i] = light ? UI_BG_SOLID : backgrounds[i].scrim;
    theme->background[UI_BG_STATUS] = UI_BG_COVER_TINT;
    if (light)
        theme->style[UI_STYLE_LIST_SCROLL] = UI_LIST_SCROLL_GLIDE;
    theme->motion_scale = UI_THEME_MOTION_ONE;
    for (size_t i = 0; i < UI_MOTION_COUNT; i++)
        theme->motion[i] = UI_MOTION_DEFAULT;
    if (id == UI_THEME_CLASSIC || id == UI_THEME_PLASTIC)
        classic(theme);
    if (id == UI_THEME_PLASTIC)
        plastic(theme);
}

const char *ui_theme_builtin_name(enum ui_theme_builtin_id id)
{
    return (unsigned)id < UI_THEME_BUILTIN_COUNT ? builtin_names[id] : NULL;
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        int ca = (a >> shift) & 255, cb = (b >> shift) & 255;
        out |= (uint32_t)(ca + (cb - ca) * num / den) << shift;
    }
    return out;
}

uint32_t ui_theme_gradient_at(const struct ui_theme_gradient *gradient,
                              int y, int height, uint32_t fallback)
{
    if (!gradient || !gradient->stops || height <= 0)
        return fallback;
    if (gradient->rows)
    {
        int last = gradient->nrows - 1;
        if (y <= 0 || height == 1)
            return gradient->rows[0];
        if (y >= height - 1)
            return gradient->rows[last];
        if (height == 3 || last < 3)
            return gradient->rows[y < last ? y : last];
        return gradient->rows[1 + (y - 1) * (last - 2) / (height - 3)];
    }
    int first = 0, last = height - 1;
    if (gradient->stops == 4 && height >= 3)
    {
        if (y <= 0)
            return gradient->color[0];
        if (y >= height - 1)
            return gradient->color[3];
        first = 1;
        last = height - 2;
    }
    const uint32_t *ends = gradient->stops == 4 ? gradient->color + 1 : gradient->color;
    if (last <= first)
        return ends[0];
    y = y < first ? first : y > last ? last : y;
    return mix_rgb(ends[0], ends[1], y - first, last - first);
}

unsigned ui_theme_builtin_metric(enum ui_metric_role role)
{
    return metrics[role].value;
}

const char *ui_theme_builtin_font(enum ui_font_role role)
{
    return fonts[role].name;
}

/* The input is a sized span from the caller's file allocation. */
struct span {
    const char *text;
    size_t len;
};

static bool space(char c)
{
    return c == ' ' || c == '\t';
}

static struct span trim(struct span value)
{
    while (value.len && space(*value.text))
    {
        value.text++;
        value.len--;
    }
    while (value.len && space(value.text[value.len - 1]))
        value.len--;
    return value;
}

static bool same(struct span value, const char *literal)
{
    return value.len == strlen(literal) && !memcmp(value.text, literal, value.len);
}

static bool split(struct span *before, char separator, struct span *after)
{
    const char *at = memchr(before->text, separator, before->len);
    if (!at)
        return false;

    size_t offset = at - before->text;
    *after = trim((struct span){ at + 1, before->len - offset - 1 });
    *before = trim((struct span){ before->text, offset });
    return true;
}

static bool number(struct span value, unsigned min, unsigned max, uint16_t *out)
{
    if (!value.len)
        return false;

    unsigned n = 0;
    for (size_t i = 0; i < value.len; i++)
    {
        char c = value.text[i];
        if (c < '0' || c > '9')
            return false;
        n = n * 10 + (c - '0');
        if (n > max)
            return false;
    }
    if (n < min)
        return false;

    *out = n;
    return true;
}

static bool rgb(struct span value, uint32_t *out)
{
    if (value.len == 7 && value.text[0] == '#')
    {
        value.text++;
        value.len--;
    }
    if (value.len != 6)
        return false;

    uint32_t result = 0;
    for (size_t i = 0; i < value.len; i++)
    {
        unsigned digit;
        char c = value.text[i];
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;
        result = (result << 4) | digit;
    }
    *out = result;
    return true;
}

static bool palette_rule(struct span value, struct ui_theme_dynamic *out)
{
    if (same(value, "none"))
    {
        *out = (struct ui_theme_dynamic){0};
        return true;
    }

    struct span lower, upper;
    if (!split(&value, ',', &lower) || !split(&lower, ',', &upper))
        return false;

    enum ui_palette_source source;
    if (same(value, "dominant"))
        source = UI_PALETTE_DOMINANT;
    else if (same(value, "accent"))
        source = UI_PALETTE_ACCENT;
    else if (same(value, "vivid"))
        source = UI_PALETTE_VIVID;
    else
        return false;

    uint16_t min, max;
    if (!number(lower, 0, 100, &min) || !number(upper, min, 100, &max))
        return false;

    *out = (struct ui_theme_dynamic){ source, min, max };
    return true;
}

static bool parse_font_basename(struct span value, char out[UI_THEME_FONT_NAME_MAX])
{
    if (!value.len || value.len >= UI_THEME_FONT_NAME_MAX)
        return false;

    for (size_t i = 0; i < value.len; i++)
    {
        char c = value.text[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                  || (c >= 'A' && c <= 'Z');
        if (!alnum && (!i || (c != '-' && c != '_')))
            return false;
    }

    memset(out, 0, UI_THEME_FONT_NAME_MAX);
    memcpy(out, value.text, value.len);
    return true;
}

static bool bg_kind(struct span value, uint8_t *out)
{
    if (same(value, "solid"))
        *out = UI_BG_SOLID;
    else if (same(value, "blurred_cover"))
        *out = UI_BG_BLURRED_COVER;
    else if (same(value, "cover_scrim"))
        *out = UI_BG_COVER_SCRIM;
    else
        return false;
    return true;
}

struct seen_tokens {
    bool color[UI_COLOR_COUNT], palette[UI_COLOR_COUNT], font[UI_FONT_COUNT];
    bool metric[UI_METRIC_COUNT], background[UI_BG_COUNT], motion;
    bool gradient[UI_GRADIENT_COUNT], style[UI_STYLE_COUNT];
    bool duration[UI_MOTION_COUNT], easing;
};

static bool keyword(struct span value, const char *const *list, uint8_t *out)
{
    for (uint8_t i = 0; list[i]; i++)
        if (same(value, list[i]))
        {
            *out = i;
            return true;
        }
    return false;
}

static bool gradient_rule(struct span value, struct ui_theme_gradient *out)
{
    struct ui_theme_gradient parsed = {0};
    if (same(value, "none"))
    {
        *out = parsed;
        return true;
    }
    for (;;)
    {
        struct span rest;
        bool more = split(&value, ',', &rest);
        if (parsed.stops == 4 || !rgb(value, &parsed.color[parsed.stops++]))
            return false;
        if (!more)
            break;
        value = rest;
    }
    if (parsed.stops != 2 && parsed.stops != 4)
        return false;
    *out = parsed;
    return true;
}

static bool first_token(bool *seen)
{
    bool first = !*seen;
    *seen = true;
    return first;
}

static bool parse_token(struct ui_theme *theme, struct seen_tokens *seen,
                        struct span key, struct span value)
{
    if (same(key, "motion_scale"))
        return first_token(&seen->motion) && number(value, 0, 300, &theme->motion_scale);
    if (same(key, "motion_easing"))
        return first_token(&seen->easing) && keyword(value, easings, &theme->easing);

    struct span role;
    if (!split(&key, '.', &role))
        return false;

    if (same(key, "color"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(colors); i++)
            if (same(role, colors[i].key))
                return first_token(&seen->color[i]) && rgb(value, &theme->color[i]);
    }
    else if (same(key, "palette"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(colors); i++)
            if (same(role, colors[i].key))
                return first_token(&seen->palette[i]) && palette_rule(value, &theme->dynamic[i]);
    }
    else if (same(key, "font"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(fonts); i++)
            if (same(role, fonts[i].key))
                return first_token(&seen->font[i]) && parse_font_basename(value, theme->font[i]);
    }
    else if (same(key, "metric"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(metrics); i++)
            if (same(role, metrics[i].key))
                return first_token(&seen->metric[i]) &&
                       number(value, metrics[i].min, metrics[i].max, &theme->metric[i]);
    }
    else if (same(key, "gradient"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(gradients); i++)
            if (same(role, gradients[i]))
                return first_token(&seen->gradient[i]) &&
                       gradient_rule(value, &theme->gradient[i]);
    }
    else if (same(key, "style"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(styles); i++)
            if (same(role, styles[i].key))
                return first_token(&seen->style[i]) &&
                       keyword(value, styles[i].keywords, &theme->style[i]);
    }
    else if (same(key, "motion"))
    {
        for (size_t i = 0; i < ARRAY_COUNT(motions); i++)
            if (same(role, motions[i]))
                return first_token(&seen->duration[i]) &&
                       number(value, 0, 120, &theme->motion[i]);
    }
    else if (same(key, "background"))
    {
        if (same(role, "status"))
        {
            if (!first_token(&seen->background[UI_BG_STATUS]))
                return false;
            if (same(value, "cover_tint"))
                theme->background[UI_BG_STATUS] = UI_BG_COVER_TINT;
            else if (same(value, "solid"))
                theme->background[UI_BG_STATUS] = UI_BG_SOLID;
            else
                return false;
            return true;
        }
        for (size_t i = 0; i < ARRAY_COUNT(backgrounds); i++)
            if (same(role, backgrounds[i].key))
                return first_token(&seen->background[i]) && bg_kind(value, &theme->background[i]);
    }
    return false;
}

static bool layout_fits(const uint16_t *m)
{
    unsigned list_insets = 2 * m[UI_METRIC_LIST_X] + m[UI_METRIC_LIST_ICON_MARGIN]
                         + m[UI_METRIC_LIST_ICON_SIZE] + m[UI_METRIC_LIST_ICON_GAP]
                         + m[UI_METRIC_LIST_TEXT_PADDING] + m[UI_METRIC_LIST_RIGHT_PADDING]
                         + m[UI_METRIC_LIST_ACCESSORY_GAP] + m[UI_METRIC_LIST_SELECTION_WIDTH];
    unsigned status_width = m[UI_METRIC_STATUS_PAD_LEFT] + m[UI_METRIC_STATUS_PAD_RIGHT]
                          + 3 * m[UI_METRIC_STATUS_ICON_SIZE] + m[UI_METRIC_STATUS_ICON_GAP]
                          + m[UI_METRIC_STATUS_GROUP_GAP] + m[UI_METRIC_STATUS_LINK_GAP]
                          + m[UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH];
    unsigned modal_inset = m[UI_METRIC_MODAL_EDGE_MARGIN] + m[UI_METRIC_MODAL_PADDING]
                         + m[UI_METRIC_MODAL_BORDER_WIDTH];
    unsigned handoff_button_y = LCD_HEIGHT - m[UI_METRIC_HANDOFF_MARGIN]
                              - m[UI_METRIC_HANDOFF_BUTTON_HEIGHT];
    unsigned handoff_outline = m[UI_METRIC_HANDOFF_OUTLINE_WIDTH];

    return list_insets < LCD_WIDTH &&
           m[UI_METRIC_LIST_Y] >= m[UI_METRIC_STATUS_HEIGHT] &&
           m[UI_METRIC_LIST_Y] + m[UI_METRIC_LIST_BOTTOM] + m[UI_METRIC_ROW_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_ROW_HEIGHT] >= m[UI_METRIC_LIST_ICON_SIZE] &&
           m[UI_METRIC_ROW_HEIGHT] >= m[UI_METRIC_LIST_TEXT_HEIGHT] &&
           status_width < LCD_WIDTH &&
           m[UI_METRIC_STATUS_CLOCK_WIDTH] <= m[UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH] &&
           m[UI_METRIC_STATUS_TEXT_Y] + m[UI_METRIC_STATUS_TITLE_HEIGHT] <= m[UI_METRIC_STATUS_HEIGHT] &&
           m[UI_METRIC_STATUS_ICON_Y] + m[UI_METRIC_STATUS_ICON_SIZE] <= m[UI_METRIC_STATUS_HEIGHT] &&
           2 * modal_inset + 4 * m[UI_METRIC_MODAL_BUTTON_PAD_X] + m[UI_METRIC_MODAL_BUTTON_GAP] < LCD_WIDTH &&
           2 * modal_inset + 2 * m[UI_METRIC_MODAL_BUTTON_PAD_Y] < LCD_HEIGHT &&
           m[UI_METRIC_WPS_TRANSPORT_X] + m[UI_METRIC_WPS_TRANSPORT_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_TRANSPORT_Y] + m[UI_METRIC_WPS_TRANSPORT_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_TITLE_X] + m[UI_METRIC_WPS_TITLE_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_TITLE_Y] + m[UI_METRIC_WPS_TITLE_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_SUBTITLE_Y] + m[UI_METRIC_WPS_SUBTITLE_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_PROGRESS_Y] + m[UI_METRIC_WPS_PROGRESS_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_DECK_INDEX_X] + m[UI_METRIC_WPS_DECK_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_DECK_TIME_X] + m[UI_METRIC_WPS_DECK_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_DECK_Y] + m[UI_METRIC_WPS_DECK_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_METER_X] + m[UI_METRIC_WPS_METER_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_METER_Y] + m[UI_METRIC_WPS_METER_HEIGHT] <= LCD_HEIGHT &&
           m[UI_METRIC_WPS_METER_PEAK_HEIGHT] <= m[UI_METRIC_WPS_METER_HEIGHT] &&
           m[UI_METRIC_WPS_PANEL_TOP] < m[UI_METRIC_WPS_PROGRESS_Y] &&
           m[UI_METRIC_WPS_VOLUME_X] + m[UI_METRIC_WPS_VOLUME_WIDTH] <= LCD_WIDTH &&
           m[UI_METRIC_WPS_VOLUME_Y] + m[UI_METRIC_WPS_VOLUME_HEIGHT] <= m[UI_METRIC_STATUS_HEIGHT] &&
           m[UI_METRIC_HANDOFF_COVER_SIZE] + m[UI_METRIC_HANDOFF_METADATA_GAP]
               + 2u * m[UI_METRIC_HANDOFF_LINE_GAP] < handoff_button_y &&
           m[UI_METRIC_HANDOFF_PROGRESS_GAP] + m[UI_METRIC_HANDOFF_PROGRESS_HEIGHT]
               <= m[UI_METRIC_HANDOFF_MARGIN] &&
           2u * handoff_outline <= m[UI_METRIC_HANDOFF_BUTTON_HEIGHT] &&
           2u * m[UI_METRIC_HANDOFF_MARGIN] + m[UI_METRIC_HANDOFF_BUTTON_GAP]
               + 4u * handoff_outline < LCD_WIDTH &&
           m[UI_METRIC_HANDOFF_ERROR_OFFSET] <= handoff_button_y;
}

bool ui_theme_parse(struct ui_theme *theme, const char *data, size_t size)
{
    if (!theme || !data || !size || size > UI_THEME_FILE_MAX)
        return false;

    struct ui_theme parsed;
    struct seen_tokens seen = {0};
    bool version = false;
    ui_theme_builtin(&parsed, UI_THEME_SCRIM);
    parsed.background[UI_BG_STATUS] = UI_BG_SOLID;

    while (size)
    {
        const char *end = memchr(data, '\n', size);
        size_t len = end ? (size_t)(end - data) : size;
        struct span line = { data, len };
        size_t used = len + (end != NULL);
        data += used;
        size -= used;

        if (line.len && line.text[line.len - 1] == '\r')
            line.len--;
        for (size_t i = 0; i < line.len; i++)
        {
            unsigned char c = line.text[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return false;
        }
        line = trim(line);
        if (!line.len || line.text[0] == '#')
            continue;

        struct span value;
        if (!split(&line, '=', &value))
            return false;
        if (!version)
        {
            if (!same(line, "podbox-theme") || !same(value, "1"))
                return false;
            version = true;
        }
        else if (!parse_token(&parsed, &seen, line, value))
            return false;
    }

    if (!version || !layout_fits(parsed.metric))
        return false;
    if (parsed.style[UI_STYLE_ART] == UI_ART_PLASTIC)
        plastic_columns(&parsed);

    *theme = parsed;
    return true;
}

static unsigned pack_color(uint32_t rgb888)
{
    return LCD_RGBPACK((rgb888 >> 16) & 255, (rgb888 >> 8) & 255, rgb888 & 255);
}

static uint32_t unpack_color(unsigned packed)
{
    return (uint32_t)RGB_UNPACK_RED(packed) << 16 |
           (uint32_t)RGB_UNPACK_GREEN(packed) << 8 |
           (uint32_t)RGB_UNPACK_BLUE(packed);
}

static bool palette_color(const struct ui_album_palette *palette,
                          struct ui_theme_dynamic rule, uint32_t *color)
{
    switch (rule.source)
    {
        case UI_PALETTE_DOMINANT:
            *color = palette->dominant;
            return palette->has_dominant;
        case UI_PALETTE_ACCENT:
            *color = palette->accent;
            return palette->has_accent;
        case UI_PALETTE_VIVID:
            if (palette->has_vivid)
            {
                *color = palette->vivid;
                return true;
            }
            if (palette->has_accent && palette->has_dominant &&
                (rule.min_tone > 0 || rule.max_tone < 100))
            {
                unsigned black = pack_color(0);
                bool accent_lighter = color_contrast(pack_color(palette->accent), black) >
                                      color_contrast(pack_color(palette->dominant), black);
                *color = (rule.min_tone > 0) == accent_lighter ? palette->accent : palette->dominant;
                return true;
            }
            *color = palette->has_accent ? palette->accent : palette->dominant;
            return palette->has_accent || palette->has_dominant;
        default:
            return false;
    }
}

uint32_t ui_theme_color(const struct ui_theme *theme, enum ui_color_role role,
                        const struct ui_album_palette *palette)
{
    if ((unsigned)role >= UI_COLOR_COUNT)
        return 0;

    uint32_t rgb888;
    struct ui_theme_dynamic rule = theme ? theme->dynamic[role] : colors[role].palette;
    if (!palette || !palette_color(palette, rule, &rgb888))
        return theme ? theme->color[role] : colors[role].scrim;

    unsigned original = pack_color(rgb888);
    unsigned bounded = original;
    if (rule.min_tone)
        bounded = color_tone_bound(bounded, rule.min_tone, 1);
    if (rule.max_tone < 100)
        bounded = color_tone_bound(bounded, rule.max_tone, -1);
    return bounded == original ? rgb888 : unpack_color(bounded);
}
