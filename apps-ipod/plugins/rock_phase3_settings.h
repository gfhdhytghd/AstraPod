/* Native services for the Phase 3 utility plugins. GPL-2.0-or-later. */
#ifndef ROCK_PHASE3_SETTINGS_H
#define ROCK_PHASE3_SETTINGS_H

#include <stdbool.h>

struct eq_band_setting;

bool rock_reveal_file(const char *path, bool directory);
bool rock_apply_eq(const struct eq_band_setting *bands, int precut);

#endif
