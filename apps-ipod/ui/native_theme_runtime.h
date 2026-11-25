/* PodBox native theme loading and Settings UI. GPL version 2+. */
#ifndef PODBOX_NATIVE_THEME_RUNTIME_H
#define PODBOX_NATIVE_THEME_RUNTIME_H

#include "native_theme.h"

#define UI_THEME_NAME_MAX 32
#define UI_THEME_FONT_PATH_MAX \
    (sizeof(FONT_DIR "/.pbf") + UI_THEME_FONT_NAME_MAX)

void ui_theme_snapshot(struct ui_theme *out);
unsigned ui_theme_generation(void);
unsigned ui_theme_metric(enum ui_metric_role role);
unsigned ui_theme_rgb565(enum ui_color_role role);
unsigned ui_theme_duration(unsigned frames);
/* ui_theme_duration() of `frames`, first rescaled so that `base` frames
 * last the theme's duration for `role` when the theme sets one. */
unsigned ui_theme_motion(enum ui_motion_role role, unsigned frames,
                         unsigned base);
/* The current theme's keyword for `role`; 0 without a theme. */
unsigned ui_theme_style(enum ui_style_role role);
/* The current theme's UI_EASE_* curve. */
unsigned ui_theme_easing(void);
int ui_theme_font_load(const struct ui_theme *, enum ui_font_role, int glyphs);
int ui_theme_current_font_load(enum ui_font_role role, int glyphs);
/* The file the current theme names for `role`. */
void ui_theme_current_font_path(enum ui_font_role role, char *path,
                                size_t size);
void ui_theme_apply_settings(void);
bool ui_theme_set(const char *name, bool fallback);
int ui_theme_choose(void);

#endif
