/* PodBox native theme loading and Settings UI. GPL version 2+. */
#ifndef PODBOX_NATIVE_THEME_RUNTIME_H
#define PODBOX_NATIVE_THEME_RUNTIME_H

#include "native_theme.h"

#define UI_THEME_NAME_MAX 32

void ui_theme_snapshot(struct ui_theme *out);
unsigned ui_theme_generation(void);
unsigned ui_theme_metric(enum ui_metric_role role);
unsigned ui_theme_rgb565(enum ui_color_role role);
unsigned ui_theme_duration(unsigned frames);
int ui_theme_font_load(const struct ui_theme *, enum ui_font_role, int glyphs);
int ui_theme_current_font_load(enum ui_font_role role, int glyphs);
void ui_theme_apply_settings(void);
bool ui_theme_set(const char *name, bool fallback);
int ui_theme_choose(void);

#endif
