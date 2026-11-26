/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PODBOX_NATIVE_THEME_H
#define PODBOX_NATIVE_THEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_THEME_FONT_NAME_MAX 32
#define UI_THEME_FILE_MAX 4096
#define UI_THEME_MOTION_ONE 100

enum ui_color_role {
    UI_COLOR_BACKGROUND, UI_COLOR_FOREGROUND, UI_COLOR_MUTED,
    UI_COLOR_ACCENT, UI_COLOR_SELECTION, UI_COLOR_SELECTED_TEXT,
    UI_COLOR_SEPARATOR, UI_COLOR_DIM,
    UI_COLOR_STATUS_BG, UI_COLOR_STATUS_FG,
    UI_COLOR_MODAL_BG, UI_COLOR_MODAL_FG, UI_COLOR_MODAL_BORDER,
    UI_COLOR_MODAL_ACCENT, UI_COLOR_MODAL_SELECTED_TEXT, UI_COLOR_MODAL_SHADOW,
    UI_COLOR_WPS_BG, UI_COLOR_WPS_FG, UI_COLOR_WPS_MUTED, UI_COLOR_WPS_ACCENT,
    UI_COLOR_WPS_METER, UI_COLOR_WPS_PANEL, UI_COLOR_LIST_TINT,
    UI_COLOR_HANDOFF_BG, UI_COLOR_HANDOFF_FG, UI_COLOR_HANDOFF_MUTED,
    UI_COLOR_HANDOFF_UNSELECTED_TEXT, UI_COLOR_HANDOFF_SELECTED_TEXT,
    UI_COLOR_HANDOFF_COVER_PLACEHOLDER,
    UI_COLOR_SELECTED_SECONDARY,
    UI_COLOR_COUNT
};

enum ui_font_role {
    UI_FONT_LIST, UI_FONT_TITLE, UI_FONT_BODY, UI_FONT_LYRICS,
    UI_FONT_STATUS, UI_FONT_ICON, UI_FONT_WPS_TITLE, UI_FONT_WPS_META,
    UI_FONT_HANDOFF_CAPTION, UI_FONT_HANDOFF_TITLE,
    UI_FONT_LYRICS_FOCUS,
    UI_FONT_LYRICS_BACKGROUND,
    UI_FONT_PANEL,
    UI_FONT_STATUS_TITLE,
    UI_FONT_COUNT
};

enum ui_metric_role {
    UI_METRIC_ROW_HEIGHT, UI_METRIC_LIST_X, UI_METRIC_LIST_Y,
    UI_METRIC_LIST_BOTTOM, UI_METRIC_LIST_ICON_SIZE, UI_METRIC_LIST_ICON_MARGIN,
    UI_METRIC_LIST_ICON_GAP, UI_METRIC_LIST_TEXT_PADDING,
    UI_METRIC_LIST_TEXT_HEIGHT, UI_METRIC_LIST_RIGHT_PADDING,
    UI_METRIC_LIST_SELECTION_WIDTH, UI_METRIC_LIST_SELECTION_ALPHA,
    UI_METRIC_LIST_ACCESSORY_GAP, UI_METRIC_LIST_TINT_ALPHA,
    UI_METRIC_STATUS_HEIGHT, UI_METRIC_STATUS_PAD_LEFT,
    UI_METRIC_STATUS_PAD_RIGHT, UI_METRIC_STATUS_ICON_SIZE,
    UI_METRIC_STATUS_ICON_GAP, UI_METRIC_STATUS_GROUP_GAP,
    UI_METRIC_STATUS_CLOCK_WIDTH, UI_METRIC_STATUS_CLOCK_NUMERIC_WIDTH,
    UI_METRIC_STATUS_LINK_GAP, UI_METRIC_STATUS_TEXT_Y,
    UI_METRIC_STATUS_TITLE_HEIGHT, UI_METRIC_STATUS_ICON_Y,
    UI_METRIC_MODAL_PADDING, UI_METRIC_MODAL_EDGE_MARGIN,
    UI_METRIC_MODAL_SHADOW_OFFSET, UI_METRIC_MODAL_BORDER_WIDTH,
    UI_METRIC_MODAL_BUTTON_GAP, UI_METRIC_MODAL_BUTTON_PAD_X,
    UI_METRIC_MODAL_BUTTON_PAD_Y, UI_METRIC_COVER_SIZE,
    UI_METRIC_WPS_TRANSPORT_X, UI_METRIC_WPS_TRANSPORT_Y,
    UI_METRIC_WPS_TRANSPORT_WIDTH, UI_METRIC_WPS_TRANSPORT_HEIGHT,
    UI_METRIC_WPS_TITLE_X, UI_METRIC_WPS_TITLE_Y,
    UI_METRIC_WPS_TITLE_WIDTH, UI_METRIC_WPS_TITLE_HEIGHT,
    UI_METRIC_WPS_SUBTITLE_Y, UI_METRIC_WPS_SUBTITLE_HEIGHT,
    UI_METRIC_WPS_PROGRESS_WIDTH,
    UI_METRIC_WPS_PROGRESS_HEIGHT, UI_METRIC_WPS_PROGRESS_Y,
    UI_METRIC_WPS_PROGRESS_ALPHA, UI_METRIC_WPS_DECK_INDEX_X,
    UI_METRIC_WPS_DECK_TIME_X, UI_METRIC_WPS_DECK_Y,
    UI_METRIC_WPS_DECK_WIDTH, UI_METRIC_WPS_DECK_HEIGHT,
    UI_METRIC_WPS_METER_X, UI_METRIC_WPS_METER_Y,
    UI_METRIC_WPS_METER_WIDTH, UI_METRIC_WPS_METER_HEIGHT,
    UI_METRIC_WPS_METER_PEAK_HEIGHT,
    UI_METRIC_HANDOFF_MARGIN, UI_METRIC_HANDOFF_COVER_SIZE,
    UI_METRIC_HANDOFF_BUTTON_HEIGHT, UI_METRIC_HANDOFF_BUTTON_GAP,
    UI_METRIC_HANDOFF_METADATA_GAP, UI_METRIC_HANDOFF_LINE_GAP,
    UI_METRIC_HANDOFF_PROGRESS_GAP, UI_METRIC_HANDOFF_PROGRESS_HEIGHT,
    UI_METRIC_HANDOFF_SCRIM_ALPHA, UI_METRIC_HANDOFF_OUTLINE_WIDTH,
    UI_METRIC_HANDOFF_COVER_BORDER_ALPHA, UI_METRIC_HANDOFF_BUTTON_BORDER_ALPHA,
    UI_METRIC_HANDOFF_PROGRESS_ALPHA, UI_METRIC_HANDOFF_ERROR_OFFSET,
    UI_METRIC_SPLASH_PROGRESS_GAP, UI_METRIC_SPLASH_PROGRESS_RADIUS,
    UI_METRIC_WPS_PANEL_TOP, UI_METRIC_WPS_PANEL_ALPHA,
    UI_METRIC_WPS_VOLUME_X, UI_METRIC_WPS_VOLUME_Y,
    UI_METRIC_WPS_VOLUME_WIDTH, UI_METRIC_WPS_VOLUME_HEIGHT,
    UI_METRIC_LIST_ART_SIZE, UI_METRIC_LIST_ART_ROW_HEIGHT,
    UI_METRIC_LIST_ART_TOP_GAP, UI_METRIC_LIST_ART_BOTTOM_GAP,
    UI_METRIC_LIST_HEADER_HEIGHT, UI_METRIC_LIST_HEADER_BOTTOM_GAP,
    UI_METRIC_LIST_NOTES_HEIGHT, UI_METRIC_LIST_AUX_PADDING,
    UI_METRIC_COUNT
};

enum ui_bg_role { UI_BG_LIST, UI_BG_WPS, UI_BG_LYRICS, UI_BG_HANDOFF,
                  UI_BG_STATUS, UI_BG_COUNT };
enum ui_bg_kind { UI_BG_SOLID, UI_BG_BLURRED_COVER, UI_BG_COVER_SCRIM,
                  UI_BG_COVER_TINT };

/* Optional gradients that replace a solid fill: top to bottom, except the
 * scroll thumb, which is shaded left to right. */
enum ui_gradient_role {
    UI_GRADIENT_SELECTION, UI_GRADIENT_STATUS, UI_GRADIENT_PROGRESS,
    UI_GRADIENT_PROGRESS_TRACK, UI_GRADIENT_SCROLLBAR, UI_GRADIENT_COUNT
};

/* Keyword tokens. Value 0 is each style's Scrim behaviour. */
enum ui_style_role {
    UI_STYLE_SELECTION, UI_STYLE_LIST_ICONS, UI_STYLE_ACCESSORY,
    UI_STYLE_STATUS_CLOCK, UI_STYLE_STATUS_TITLE, UI_STYLE_PAGE_MOTION,
    UI_STYLE_WPS_LAYOUT, UI_STYLE_SCROLLBAR, UI_STYLE_ART,
    UI_STYLE_LIST_SCROLL, UI_STYLE_COUNT
};
enum { UI_SELECTION_MARKER, UI_SELECTION_BAR };
enum { UI_LIST_ICONS_AUTO, UI_LIST_ICONS_OFF };
enum { UI_ACCESSORY_ALL, UI_ACCESSORY_SELECTED };
enum { UI_STATUS_CLOCK_ON, UI_STATUS_CLOCK_OFF };
enum { UI_STATUS_TITLE_LEFT, UI_STATUS_TITLE_CENTER };
enum { UI_PAGE_MOTION_CASCADE, UI_PAGE_MOTION_SLIDE };
enum { UI_WPS_LAYOUT_COVER, UI_WPS_LAYOUT_CLASSIC };
enum { UI_SCROLLBAR_PLAIN, UI_SCROLLBAR_CLASSIC };
enum { UI_ART_NONE, UI_ART_PLASTIC };
enum { UI_LIST_SCROLL_SNAP, UI_LIST_SCROLL_GLIDE };

/* Per-transition durations in 60 Hz frames; UI_MOTION_DEFAULT keeps the
 * engine's own. */
enum ui_motion_role {
    UI_MOTION_PUSH, UI_MOTION_POP, UI_MOTION_LIST, UI_MOTION_SHEET_IN,
    UI_MOTION_SHEET_OUT, UI_MOTION_COUNT
};
#define UI_MOTION_DEFAULT 0xffffu

/* Easing of page transitions. */
enum { UI_EASE_CUBIC, UI_EASE_QUAD, UI_EASE_QUINT, UI_EASE_SINE,
       UI_EASE_LINEAR };

enum ui_theme_builtin_id {
    UI_THEME_SCRIM, UI_THEME_LIGHT, UI_THEME_CLASSIC, UI_THEME_PLASTIC,
    UI_THEME_BUILTIN_COUNT
};

/* stops is 0 (no gradient), 2 (top and bottom) or 4 (a one-pixel top line,
 * top and bottom of the body, a one-pixel bottom line). `rows`, when set,
 * replaces the stops with `nrows` exact RGB888 rows, stretched through the
 * body and keeping the first and last as the edge lines. */
struct ui_theme_gradient {
    uint32_t color[4];
    uint8_t stops;
    uint8_t nrows;
    const uint32_t *rows;
};

enum ui_palette_source {
    UI_PALETTE_NONE, UI_PALETTE_DOMINANT, UI_PALETTE_ACCENT, UI_PALETTE_VIVID
};

struct ui_theme_dynamic {
    uint8_t source;
    uint8_t min_tone;
    uint8_t max_tone;
};

/* RGB888 colours with per-source validity, since black is a valid colour. */
struct ui_album_palette {
    uint32_t dominant, accent, vivid;
    bool has_dominant, has_accent, has_vivid;
};

struct ui_theme {
    uint32_t color[UI_COLOR_COUNT];
    struct ui_theme_dynamic dynamic[UI_COLOR_COUNT];
    char font[UI_FONT_COUNT][UI_THEME_FONT_NAME_MAX];
    uint16_t metric[UI_METRIC_COUNT];
    uint8_t background[UI_BG_COUNT];
    uint16_t motion_scale;
    struct ui_theme_gradient gradient[UI_GRADIENT_COUNT];
    uint8_t style[UI_STYLE_COUNT];
    uint16_t motion[UI_MOTION_COUNT];
    uint8_t easing;
};

void ui_theme_builtin(struct ui_theme *theme, enum ui_theme_builtin_id id);
/* The built-in theme's name, or NULL past the last. */
const char *ui_theme_builtin_name(enum ui_theme_builtin_id id);
/* Colour of a gradient at row y of a fill `height` rows tall, RGB888.
 * A role without a gradient returns `fallback`. */
uint32_t ui_theme_gradient_at(const struct ui_theme_gradient *gradient,
                              int y, int height, uint32_t fallback);
unsigned ui_theme_builtin_metric(enum ui_metric_role role);
const char *ui_theme_builtin_font(enum ui_font_role role);

/* Parse a complete file, using Scrim for absent tokens. Failure preserves theme. */
bool ui_theme_parse(struct ui_theme *theme, const char *data, size_t size);

/* Resolve palette roles in RGB888. NULL uses the default Scrim theme. */
uint32_t ui_theme_color(const struct ui_theme *theme, enum ui_color_role role,
                        const struct ui_album_palette *palette);

#endif
