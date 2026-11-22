/* Native services for the Phase 3 utility plugins. GPL-2.0-or-later. */
#include "config.h"
#include "file.h"
#include "dir.h"
#include "settings.h"
#include "system/applimits.h"
#include "screens/browse/browser.h"
#include "screens/settings/eq_settings.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_phase3_settings.h"
#include "plugins/rock_widgets.h"

bool rock_reveal_file(const char *path, bool directory)
{
    if (rock_usb_taken() || path[0] != '/' ||
        !browser_get_context()->is_browsing)
        return false;

    size_t length = strlen(path);
    char selection[MAX_PATH];
    if (length >= sizeof(selection) - directory)
        return false;

    unsigned depth = 0;
    for (const char *p = path + 1; *p; p++)
        depth += *p == '/';
    if (depth + directory >= MAX_DIR_LEVELS)
        return false;

    if (directory ? !dir_exists(path) : !file_exists(path))
        return false;

    memcpy(selection, path, length + 1);
    if (directory && selection[length - 1] != '/')
    {
        selection[length++] = '/';
        selection[length] = '\0';
    }

    set_current_file(selection);
    return true;
}

bool rock_apply_eq(const struct eq_band_setting *bands, int precut)
{
    if (rock_usb_taken() || precut < 0 || precut > 240)
        return false;

    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        const struct eq_band_setting *band = &bands[i];
        if ((unsigned)band->type > EQ_FILTER_HIGH_SHELF ||
            band->cutoff < EQ_CUTOFF_MIN || band->cutoff > EQ_CUTOFF_MAX ||
            band->q < EQ_Q_MIN || band->q > EQ_Q_MAX ||
            band->gain < EQ_GAIN_MIN || band->gain > EQ_GAIN_MAX)
            return false;
    }

    rock_flush_settings();
    memcpy(global_settings.eq_band_settings, bands,
           sizeof(global_settings.eq_band_settings));
    global_settings.eq_precut = precut;
    eq_apply();
    int result = settings_save();
    rock_begin_settings();
    return result >= 0;
}
