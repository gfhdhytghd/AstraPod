/***************************************************************************
 * Copyright (C) 2026 PodBox contributors
 * GNU General Public License (version 2+)
 *
 * Describe and commit a single native setting from PodLink Shell.
 ****************************************************************************/

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "audio.h"
#include "audio/enc_config.h"
#include "screens/recording.h"
#include "screens/context_menu.h"
#include "system/activity.h"
#include "dir.h"
#include "draw/color.h"
#include "file.h"
#include "general.h"
#include "input/action.h"
#include "root_menu.h"
#include "screens/browse/browser_disk.h"
#include "screens/browse/browser_db.h"
#include "screens/music_menu_config.h"
#include "database/tagcache.h"
#include "screens/settings/eq_settings.h"
#include "screens/settings/sound_settings.h"
#include "settings/settings.h"
#include "sound.h"
#include "speech/talk.h"
#include "system/format_time.h"
#include "system/volume.h"
#include "ui/native_theme_runtime.h"
#include "widgets/menu.h"
#include "widgets/option_select.h"
#include "shell_value.h"

static bool is_volume_limit(const struct settings_list *setting)
{
    return setting->setting == &global_settings.volume_limit;
}

static bool is_peak_range(const struct settings_list *setting)
{
    return setting->setting == &global_settings.peak_meter_min ||
           setting->setting == &global_settings.peak_meter_max;
}

static bool is_managed_file(const struct settings_list *setting)
{
    return setting->setting == global_settings.font_file ||
           setting->setting == global_settings.lang_file ||
           setting->setting == global_settings.text_viewer_font_file;
}

#ifdef HAVE_RECORDING
static bool is_recording_option(const void *variable)
{
    return variable == &global_settings.rec_source ||
           variable == &global_settings.rec_format ||
           variable == &global_settings.rec_channels ||
           variable == &global_settings.rec_mono_mode ||
           variable == &global_settings.rec_frequency ||
           variable == &global_settings.mp3_enc_config.bitrate ||
           variable == global_settings.rec_directory;
}

static void recording_choice_mask(const void *variable,
                                  struct shell_value_info *info)
{
    if (variable == &global_settings.mp3_enc_config.bitrate)
    {
        info->has_choice_mask = true;
        info->choice_mask = enc_mp3_bitrate_caps();
        info->unit = "kbit/s";
    }
    else if (variable == &global_settings.rec_frequency ||
             variable == &global_settings.rec_channels)
    {
        struct encoder_config cfg = {.rec_format = global_settings.rec_format};
        struct encoder_caps caps;
        info->has_choice_mask = true;
        if ((unsigned)cfg.rec_format >= REC_NUM_FORMATS)
            return;
        global_to_encoder_config(&cfg);
        if (!enc_get_caps(&cfg, &caps, true))
            return;
        if (variable == &global_settings.rec_channels)
            info->choice_mask = caps.channel_caps;
        else
        {
            info->unit = "kHz";
#ifdef HAVE_SPDIF_REC
            if (global_settings.rec_source == REC_SRC_SPDIF)
                return;
#endif
            unsigned long available[REC_NUM_FREQ];
            int count = make_list_from_caps32(REC_SAMPR_CAPS, NULL,
                                              caps.samplerate_caps, available);
            for (int i = 0; i < count; i++)
                info->choice_mask |= 1ul << available[i];
        }
    }
}
#endif

static bool is_hotkey(const void *variable)
{
    return variable == &global_settings.hotkey_tree;
}

static bool is_context_visibility(const void *variable)
{
    for (unsigned i = 0; i < ARRAYLEN(global_settings.context_hidden); i++)
        if (variable == &global_settings.context_hidden[i])
            return true;
    return false;
}

bool shell_value_writable(const struct settings_list *setting,
                          const struct menu_item_ex *item)
{
    if (setting->flags & F_DEPRECATED)
        return false;

#ifdef HAVE_RECORDING
    if (is_recording_option(setting->setting))
        return true;
#endif

    if (is_volume_limit(setting) || (setting->flags & F_EQSETTING) ||
        is_peak_range(setting) || is_managed_file(setting) ||
        setting->setting == global_settings.native_theme ||
        setting->setting == &global_settings.peak_meter_dbfs ||
        setting->setting == &global_settings.bl_selective_actions_mask)
        return true;

    if (is_hotkey(setting->setting) || is_context_visibility(setting->setting) ||
        setting->setting == &global_settings.root_menu_customized ||
        setting->setting == global_settings.start_directory ||
        setting->setting == global_settings.tagcache_scan_paths ||
        setting->setting == &global_settings.music_menu_hidden ||
        setting->setting == &global_settings.database_album_sort_ctx)
        return true;

    if ((setting->flags & F_T_MASK) == F_T_CUSTOM)
        return false;
    if (item)
    {
        int type = item->flags & MENU_TYPE_MASK;
        if (type != MT_SETTING && type != MT_SETTING_W_TEXT)
            return (setting->flags & F_RGB) != 0;
    }

    return true;
}

void shell_value_info(const struct settings_list *setting,
                      struct shell_value_info *info)
{
    bool volume_limit = is_volume_limit(setting);
    uint32_t flags = setting->flags;

    *info = (struct shell_value_info) {
        .type = "integer",
        .allowed = setting_get_cfgvals(setting),

    };

    if (volume_limit || (flags & F_T_SOUND))
    {
        int id = volume_limit ? SOUND_VOLUME : setting->sound_setting->setting;
        info->type = "sound";
        info->has_range = true;
        info->min = sound_min(id);
        info->max = !volume_limit && id == SOUND_VOLUME
                  ? global_settings.volume_limit : sound_max(id);
        info->step = sound_steps(id);
        info->unit = sound_unit(id);
        info->decimals = sound_numdecimals(id);
    }
    else if (is_peak_range(setting))
    {
        bool dbfs = global_settings.peak_meter_dbfs;
        bool lower = setting->setting == &global_settings.peak_meter_min;
        info->has_range = true;
        info->step = 1;
        info->unit = dbfs ? "-dBFS" : "%";
        if (dbfs)
        {
            info->min = lower ? global_settings.peak_meter_max : 0;
            info->max = lower ? 89 : global_settings.peak_meter_min;
        }
        else
        {
            info->min = lower ? 0 : global_settings.peak_meter_min + 1;
            info->max = lower ? global_settings.peak_meter_max - 1 : 100;
        }
    }
    else if (setting->setting == &global_settings.bl_selective_actions_mask)
    {
        info->type = "mask";
        info->format = "bit sum: volume=1, play=2, seek=4, skip=8, "
                       "unmapped=16, external power=512";
    }
    else if (setting->setting == &global_settings.music_menu_hidden)
    {
        info->type = "mask";
        info->format = "one bit per row index in tagnavi.config";
    }
    else if (setting->setting == &global_settings.database_album_sort_ctx)
    {
        info->type = "mask";
        info->format = "all albums, artist, album artist, composer: one three-bit "
                       "slot each (low bits at 0/2/4/6, high bits at 8/9/10/11). "
                       "0=default, 1=name, 2=year, 3=year descending, "
                       "4=artist+name, 5=artist+year, 6=artist+year descending";
    }
    else if ((flags & F_T_MASK) == F_T_BOOL)
        info->type = "boolean";
    else if (flags & (F_CHOICE_SETTING | F_HAS_CFGVALS))
        info->type = "choice";
    else if (flags & F_TABLE_SETTING)
    {
        const struct table_setting *table = setting->table_setting;
        info->type = "table";
        info->values = table->values;
        info->count = table->count;
        info->unit = (const char *)unit_strings_core[table->unit];
    }
    else if (flags & F_INT_SETTING)
    {
        const struct int_setting *range = setting->int_setting;
        info->has_range = true;
        info->min = MIN(range->min, range->max);
        info->max = MAX(range->min, range->max);
        info->step = abs(range->step);
        info->unit = (const char *)unit_strings_core[range->unit];
    }
    else if (flags & F_RGB)
        info->type = "rgb";
    else if ((flags & F_T_MASK) == F_T_CUSTOM)
    {
        info->type = "custom";
        if (flags & F_EQSETTING)
            info->format = "cutoff (Hz), Q (0.1), gain (0.1 dB), "
                           "LOW_SHELF/PEAK/HIGH_SHELF";
        else if (setting->setting == &global_settings.root_menu_customized)
            info->format = "comma-separated menu keys; prefix ! to hide, - to reset";
        else if (is_hotkey(setting->setting))
            info->format = "English action name";
        else if (is_context_visibility(setting->setting))
            info->format = "English row names separated by commas; prefix + to show";
    }
    else if ((flags & F_T_MASK) == F_T_UCHARPTR ||
             (flags & F_T_MASK) == F_T_CHARPTR)
    {
        info->type = setting->filename_setting->prefix ? "file" : "text";
        info->max_bytes = setting->filename_setting->max_len - 1;
        if (setting->setting == global_settings.tagcache_scan_paths)
            info->format = "existing absolute folders separated by colons";
    }
#ifdef HAVE_RECORDING
    recording_choice_mask(setting->setting, info);
#endif
}

static bool eq_cfg_fields(const char *value)
{
    static const struct { int min, max, step; } ranges[] = {
        { EQ_CUTOFF_MIN, EQ_CUTOFF_MAX, EQ_CUTOFF_STEP },
        { EQ_Q_MIN, EQ_Q_MAX, EQ_Q_STEP },
        { EQ_GAIN_MIN, EQ_GAIN_MAX, EQ_GAIN_STEP },
    };
    char *end;

    for (unsigned i = 0; i < ARRAYLEN(ranges); i++)
    {
        errno = 0;
        long number = strtol(value, &end, 10);
        if (end == value || *end != ',' || errno == ERANGE ||
            number < ranges[i].min || number > ranges[i].max ||
            (number - ranges[i].min) % ranges[i].step != 0)
            return false;
        value = end + 1;
    }
    while (*value == ' ')
        value++;
    return !strcasecmp(value, "LOW_SHELF") || !strcasecmp(value, "PEAK") ||
           !strcasecmp(value, "HIGH_SHELF");
}

static const char *set_managed_file(const struct settings_list *setting,
                                    const char *name)
{
    const struct filename_setting *fs = setting->filename_setting;
    char path[MAX_PATH];

    if (!name[0] || strchr(name, '/') || strchr(name, '\\') ||
        !strcmp(name, ".") || !strcmp(name, ".."))
        return "Expected a filename from the player's settings folder";

    snprintf(path, sizeof(path), "%s%s%s", fs->prefix, name, fs->suffix);
    if (!file_exists(path))
        return "The file is not installed on the player";
    if (setting->setting == global_settings.font_file ||
        setting->setting == global_settings.lang_file)
    {
        bool font = setting->setting == global_settings.font_file;
        bool loaded = font ? browser_disk_select_font(path) :
                             browser_disk_select_language(path);
        ui_set_working(false);
        return loaded ? NULL : font ? "Could not load font" :
                                     "Could not load language";
    }

    set_file(path, setting->setting);
    return NULL;
}

static bool parse_int(const char *value, int *number)
{
    char *end;
    const char *digits = value + (*value == '-' || *value == '+');

    if (*digits < '0' || *digits > '9')
        return false;

    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (*end || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        return false;

    *number = parsed;
    return true;
}

static bool parse_option(const struct settings_list *setting,
                         const struct shell_value_info *info,
                         const char *value, int *number)
{
    uint32_t flags = setting->flags;
    int index;

    if (info->allowed && cfg_string_to_int(setting, &index, value))
    {
        if (info->has_choice_mask &&
            ((unsigned)index >= sizeof(info->choice_mask) * CHAR_BIT ||
             !(info->choice_mask & (1ul << index))))
            return false;
        if ((flags & F_CHOICE_SETTING) &&
            index >= setting->choice_setting->count)
            return false;
        if (flags & F_TABLE_SETTING)
        {
            if (index >= info->count)
                return false;
            *number = info->values[index];
        }
        else
            *number = index;
        return true;
    }

    if (info->allowed && !(flags & F_TABLE_SETTING))
        return false;
    if (!parse_int(value, number))
        return false;
    if (info->has_range)
        return *number >= info->min && *number <= info->max &&
               info->step > 0 &&
               ((int64_t)*number - info->min) % info->step == 0;
    if (flags & F_TABLE_SETTING)
    {
        /* A saved off-list value can be retained in the native picker. */
        if ((flags & F_ALLOW_ARBITRARY_VALS) &&
            *number == option_value_as_int(setting))
            return true;
        for (int i = 0; i < info->count; i++)
            if (*number == info->values[i])
                return true;
        return false;
    }
    return true;
}

static void option_callback(const struct settings_list *setting,
                            int old, int number)
{
    uint32_t flags = setting->flags;
    void (*callback)(int) = NULL;

    if ((flags & F_CB_ONLY_IF_CHANGED) && number == old)
        return;

    if ((flags & F_T_MASK) == F_T_BOOL)
    {
        if (setting->bool_setting->option_callback)
            setting->bool_setting->option_callback(number != 0);
        return;
    }
    if (flags & F_T_SOUND)
        callback = sound_get_fn(setting->sound_setting->setting);
    else if (flags & F_CHOICE_SETTING)
        callback = setting->choice_setting->option_callback;
    else if (flags & F_TABLE_SETTING)
        callback = setting->table_setting->option_callback;
    else if (flags & F_INT_SETTING)
        callback = setting->int_setting->option_callback;

    if (callback)
    {
        callback(number);
        if (callback == sound_get_fn(SOUND_VOLUME))
            volume_mark_changed();
    }
}

const char *shell_value_set(const struct settings_list *setting,
                            const struct menu_item_ex *item, char *value)
{
    struct shell_value_info info;
    uint32_t flags = setting->flags;
    bool volume_limit = is_volume_limit(setting);
    int number = 0;
    int old = option_value_as_int(setting);
    char text[MAX_PATH];

    if (!shell_value_writable(setting, item))
        return "Use settings-action to open this setting on the player";

    shell_value_info(setting, &info);
    if ((flags & (F_T_MASK | F_EQSETTING)) == (F_T_CUSTOM | F_EQSETTING))
    {
        if (!eq_cfg_fields(value))
            return "Invalid EQ band fields";
        setting->custom_setting->load_from_cfg(setting->setting, value);
        eq_apply();
        settings_save();
        return NULL;
    }

    if (setting->setting == &global_settings.root_menu_customized)
    {
        if (!root_menu_validate_cfg(value))
            return "Unknown menu key or invalid root menu order";
        root_menu_load_from_cfg(setting->setting, value);
        settings_save();
        return NULL;
    }

    if (is_hotkey(setting->setting) || is_context_visibility(setting->setting))
    {
        bool valid = is_hotkey(setting->setting) ?
            hotkey_setting_from_cfg(setting->setting, value) :
            context_hidden_setting_from_cfg(setting->setting, value);
        if (!valid)
            return "Unknown action or context menu row";
        settings_save();
        return NULL;
    }

    switch (flags & F_T_MASK)
    {
        case F_T_CHARPTR:
        case F_T_UCHARPTR:
            if (!copy_filename_setting(text, sizeof(text), value,
                                       setting->filename_setting))
                return "Setting value is too long";
#ifdef HAVE_RECORDING
            if (setting->setting == global_settings.rec_directory &&
                (text[0] != '/' || !dir_exists(text)))
                return "The recording folder must be an existing absolute path";
#endif
            if (setting->setting == global_settings.start_directory &&
                (text[0] != '/' || !dir_exists(text)))
                return "The start folder must be an existing absolute path";
            if (setting->setting == global_settings.tagcache_scan_paths &&
                !tagcache_scan_paths_valid(text))
                return "Invalid database scan paths";
            if (setting->setting == global_settings.native_theme)
                return ui_theme_set(text, false) ? NULL : "Invalid native theme";
            if (is_managed_file(setting))
                return set_managed_file(setting, text);
            break;
        default:
            if (flags & F_RGB)
            {
                if (strlen(value) != 6 || hex_to_rgb(value, &number) < 0)
                    return "Expected six RGB hex digits";
                if ((setting->setting == &global_settings.text_viewer_fg &&
                     number == global_settings.text_viewer_bg) ||
                    (setting->setting == &global_settings.text_viewer_bg &&
                     number == global_settings.text_viewer_fg))
                    return "Text and page colours must differ";
            }
            else if (!parse_option(setting, &info, value, &number))
                return "Value is outside the setting's allowed values";
            break;
    }

    if (setting->setting == &global_settings.peak_meter_dbfs)
    {
        peak_meter_apply_scale(number != 0);
        settings_save();
        return NULL;
    }

    /* Volume Limit's ENTER callback runs its native number picker. */
    if (item && !volume_limit &&
        menu_item_event(ACTION_ENTER_MENUITEM, item) == ACTION_EXIT_MENUITEM)
        return "The player refused to open this setting";

    if (setting->setting == &global_settings.music_menu_hidden)
        return music_menu_set_hidden(number) ? NULL : "Invalid Music menu row mask";
    if (setting->setting == &global_settings.database_album_sort_ctx)
    {
        if (!browser_db_album_sort_set_cfg(number))
            return "Invalid album sort context mask";
        settings_save();
        return NULL;
    }
    if (setting->setting == &global_settings.bl_selective_actions_mask)
    {
        if (!backlight_set_selected_actions(number))
            return "Unsupported backlight action bits";
        settings_save();
        if (item)
            menu_item_event(ACTION_EXIT_MENUITEM, item);
        return NULL;
    }

    switch (flags & F_T_MASK)
    {
        case F_T_CHARPTR:
        case F_T_UCHARPTR:
            strcpy(setting->setting, text);
            break;
        case F_T_BOOL:
            *(bool *)setting->setting = number != 0;
            break;
        default:
            *(int *)setting->setting = number;
            break;
    }

#ifdef HAVE_RECORDING
    if ((setting->setting == &global_settings.rec_format ||
         setting->setting == &global_settings.mp3_enc_config.bitrate) &&
        old != number)
        enc_global_settings_apply();
    if (setting->setting == &global_settings.rec_trigger_mode ||
        setting->setting == &global_settings.rec_trigger_type ||
        setting->setting == &global_settings.rec_start_thres_db ||
        setting->setting == &global_settings.rec_stop_thres_db ||
        setting->setting == &global_settings.rec_start_thres_linear ||
        setting->setting == &global_settings.rec_stop_thres_linear ||
        setting->setting == &global_settings.rec_start_duration ||
        setting->setting == &global_settings.rec_stop_postrec ||
        setting->setting == &global_settings.rec_stop_gap ||
        setting->setting == &global_settings.rec_prerecord_time)
        settings_apply_trigger();
#endif
    settings_save();
    option_callback(setting, old, number);
    if (is_peak_range(setting))
        settings_apply_pm_range();
    if (item)
        menu_item_event(ACTION_EXIT_MENUITEM, item);
    else if (volume_limit)
        setvol();

    return NULL;
}
