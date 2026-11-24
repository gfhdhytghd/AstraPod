/* Test PodLink Shell against the initialized Settings menus. GPL v2 or later. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ata_idle_notify.h"
#include "podlink-shell-settings.h"
#include "rbpaths.h"
#include "screens/settings/settings_walk.h"
#include "screens/browse/browser_db.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "settings/shell_menu.h"
#include "settings/shell_value.h"
#include "system/activity.h"

static unsigned operation;
static unsigned ordinal;
static char request[SHELL_SETTINGS_OUT_SIZE];
static char reply[SHELL_SETTINGS_REPLY_SIZE + 1];

unsigned shell_settings_operation(void)
{
    return operation;
}

unsigned shell_settings_ordinal(void)
{
    return ordinal;
}

char *shell_settings_body(void)
{
    return request;
}

void shell_settings_publish(const char *json, size_t length)
{
    assert(length > 0 && length < sizeof(reply));
    memcpy(reply, json, length);
    reply[length] = '\0';
}

static void dispatch(unsigned op, const char *text)
{
    operation = op;
    assert(strlen(text) < sizeof(request));
    strcpy(request, text);
    reply[0] = '\0';
    assert(shell_settings_dispatch() == 0);
    assert(reply[0] != '\0');
}

static void expect(const char *fragment)
{
    if (!strstr(reply, fragment))
    {
        fprintf(stderr, "Expected %s in reply %s\n", fragment, reply);
        abort();
    }
}

static void value_case(const char *cfg, const char *type,
                       const char *good, const char *canonical,
                       const char *bad, const char *limit)
{
    char text[sizeof(request)];
    char expected[128];

    dispatch(SHELL_SETTINGS_GET, cfg);
    snprintf(expected, sizeof(expected), "\"type\":\"%s\"", type);
    expect(expected);
    expect("\"writable\":true");
    if (limit)
        expect(limit);

    snprintf(text, sizeof(text), "%s: %s", cfg, good);
    dispatch(SHELL_SETTINGS_SET, text);
    snprintf(expected, sizeof(expected), "\"value\":\"%s\"", canonical);
    expect(expected);
    dispatch(SHELL_SETTINGS_GET, cfg);
    expect(expected);

    snprintf(text, sizeof(text), "%s: %s", cfg, bad);
    dispatch(SHELL_SETTINGS_SET, text);
    expect("\"error\":");
    dispatch(SHELL_SETTINGS_GET, cfg);
    expect(expected);
}

static bool check_native_row(const struct menu_item_ex *item,
                             const struct settings_list *setting,
                             const char *parent, void *context)
{
    (void)item;
    (void)parent;
    unsigned *count = context;
    if (setting->cfg_name)
    {
        struct shell_menu_row row;
        assert(shell_menu_find_setting(setting, &row));
        ++*count;
    }
    return true;
}

static void test_tree(void)
{
    struct shell_menu_row row;
    bool seen[1024] = {false};
    unsigned menus = 0, actions = 0, settings_rows = 0, unique = 0, hidden = 0;
    unsigned writable = 0;
    assert((size_t)nb_settings <= sizeof(seen) / sizeof(seen[0]));

    for (ordinal = 0; shell_menu_at(ordinal, &row); ++ordinal)
    {
        assert(ordinal < 1000 && row.depth != 0 && row.id[0]);
        assert(row.path[row.depth - 1][0] != '\0');
        assert(row.path[0][0] != '\0');
        dispatch(SHELL_SETTINGS_LIST, "");
        expect("\"path\":[");
        assert(!strstr(reply, "\"error\":"));

        if (row.kind == SHELL_MENU)
            ++menus;
        if (row.kind == SHELL_SETTING)
            assert(row.setting && row.setting->cfg_name);
        if (row.kind == SHELL_ACTION)
        {
            struct shell_menu_row resolved;
            ++actions;
            assert(shell_menu_find_action(row.id, &resolved));
            assert(row.item == resolved.item);
        }
        if (!row.visible)
            ++hidden;
        if (row.setting && row.setting->cfg_name)
        {
            ++settings_rows;
            assert(!(row.setting->flags & F_DEPRECATED));
            assert(row.setting >= settings && row.setting < settings + nb_settings);
            if (!seen[row.setting - settings])
            {
                seen[row.setting - settings] = true;
                ++unique;
                writable += shell_value_writable(row.setting, row.item);
                if (!shell_value_writable(row.setting, row.item))
                    printf("Settings action-only cfg: %s\n", row.setting->cfg_name);
                struct shell_menu_row resolved;
                assert(shell_menu_find_setting(row.setting, &resolved));
                assert(resolved.setting == row.setting);
            }
        }
    }
    assert(menus && actions && settings_rows && unique && writable && hidden);
    unsigned native_rows = 0;
    settings_walk(check_native_row, &native_rows);
    assert(native_rows);
    dispatch(SHELL_SETTINGS_LIST, "");
    assert(strcmp(reply, "null") == 0);
    printf("Settings tree: %u rows, %u menus, %u actions, %u setting rows, "
           "%u unique cfg entries (%u writable), %u hidden. "
           "%u native setting rows found.\n",
           ordinal, menus, actions, settings_rows, unique, writable, hidden,
           native_rows);
    fflush(stdout);
}

#ifdef HAVE_RECORDING
static void test_recording(void)
{
    extern const struct menu_item_ex clear_rec_directory_item;
    struct shell_menu_row row;
    char action[sizeof(row.id) + 1];

    dispatch(SHELL_SETTINGS_SET, "rec format: wave");
    expect("\"value\":\"wave\"");
    dispatch(SHELL_SETTINGS_GET, "mp3_enc bitrate");
    expect("\"error\":");
    value_case("rec frequency", "choice", "44", "44", "99", "\"unit\":\"kHz\"");
    value_case("rec format", "choice", "mpa3", "mpa3", "unknown", NULL);
    value_case("mp3_enc bitrate", "choice", "16", "16", "8", "\"unit\":\"kbit/s\"");
    dispatch(SHELL_SETTINGS_GET, "rec channels");
    expect("\"allowed\":[\"mono\"]");
    expect("\"value\":\"mono\"");
    value_case("rec frequency", "choice", "22", "22", "44", "\"unit\":\"kHz\"");
    value_case("rec path", "text", ROCKBOX_DIR, ROCKBOX_DIR, "/no-such-directory", NULL);

    dispatch(SHELL_SETTINGS_SET, "peak meter dbfs: on");
    expect("\"value\":\"on\"");
    value_case("trigger start threshold dB", "integer", "-30", "-30", "1",
               "\"unit\":\"dB\"");
    dispatch(SHELL_SETTINGS_SET, "peak meter dbfs: off");
    expect("\"value\":\"off\"");
    value_case("trigger start threshold linear", "integer", "25", "25", "101",
               "\"unit\":\"%\"");

    for (ordinal = 0; shell_menu_at(ordinal, &row); ++ordinal)
        if (row.item == &clear_rec_directory_item)
            break;
    assert(row.item == &clear_rec_directory_item && row.available);
    strcpy(global_settings.rec_directory, "/old-recordings");
    snprintf(action, sizeof(action), "\x01%s", row.id);
    dispatch(SHELL_SETTINGS_ACTION, action);
    expect("\"status\":\"opened\"");
    assert(strcmp(global_settings.rec_directory, REC_BASE_DIR) == 0);
}
#endif

void __wrap_root_menu(void)
{
    global_settings.settings_mode = SETTINGS_MODE_STANDARD;
    test_tree();

    value_case("shuffle", "boolean", "on", "on", "1", "\"allowed\":[\"off\",\"on\"]");
    value_case("repeat", "choice", "one", "one", "unknown", NULL);
    value_case("scroll delay", "integer", "500", "500", "501", "\"unit\":\"ms\"");
    value_case("backlight timeout", "table", "on", "on", "9999", NULL);
    value_case("volume", "sound", "-35", "-35", "9999", "\"unit\":\"dB\"");
    dispatch(SHELL_SETTINGS_SET, "text viewer colour mode: custom");
    expect("\"value\":\"custom\"");
    value_case("text viewer text colour", "rgb", "ff0000", "ff0000", "zz0000", NULL);
    value_case("eq filter 1", "custom", "200, 10, 20, PEAK",
               "200, 10, 20, PEAK", "200, 0, 20, PEAK", NULL);
    value_case("native theme", "text", "light", "light", "missing-theme", NULL);
    value_case("lang", "file", "english", LANG_DIR "/english.lng",
               "missing-language", NULL);
    assert(!ui_working());

    value_case("hotkey tree", "custom", "Properties", "Properties, ",
               "Properties, Unknown", "\"format\":\"English action name\"");
    value_case("context hidden reading", "custom", "Show in Files",
               "Show in Files", "+Show in Files, Unknown", NULL);
    value_case("Selective Backlight Actions", "mask", "513", "513", "256", NULL);
    assert(global_settings.bl_selective_actions);
    dispatch(SHELL_SETTINGS_SET, "Selective Backlight Actions: 512");
    expect("\"value\":\"512\"");
    assert(!global_settings.bl_selective_actions);

    dispatch(SHELL_SETTINGS_GET, "font");
    expect("\"type\":\"file\"");
    dispatch(SHELL_SETTINGS_SET, "font: missing-test-font");
    expect("\"error\":");

    value_case("start directory", "text", ROCKBOX_DIR, ROCKBOX_DIR,
               "/missing-start-directory", NULL);
    dispatch(SHELL_SETTINGS_SET, "database scan paths:");
    expect("\"value\":\"\"");
    value_case("database scan paths", "text", "/:" ROCKBOX_DIR,
               "/:" ROCKBOX_DIR, "/:relative", NULL);
    value_case("database album sort contexts", "mask", "1", "1",
               "4095", NULL);
    int music_index;
    assert(browser_db_get_root_row(0, &music_index, NULL));
    assert(music_index >= 0 && music_index < 31);
    char music_mask[16];
    snprintf(music_mask, sizeof(music_mask), "%u", 1u << music_index);
    value_case("music menu hidden", "mask", music_mask, music_mask,
               "-1", NULL);
    assert(browser_db_root_row_is_hidden(music_index));
    dispatch(SHELL_SETTINGS_SET, "root menu order: settings,");
    expect("\"writable\":true");
    expect("!files, ");
    expect("settings, \"");
    dispatch(SHELL_SETTINGS_SET, "root menu order: unknown-option");
    expect("\"error\":");
    dispatch(SHELL_SETTINGS_SET, "root menu order: -");
    expect("\"writable\":true");

    dispatch(SHELL_SETTINGS_GET, "eq peak filter 1");
    expect("\"error\":");
    dispatch(SHELL_SETTINGS_ACTION, "/no-confirmation");
    expect("\"error\":");
    dispatch(SHELL_SETTINGS_ACTION, "\x01/no-such-action");
    expect("\"error\":");

#ifdef HAVE_RECORDING
    test_recording();
#endif

    call_storage_idle_notifys(true);
    global_settings.scroll_delay = 1000;
    global_settings.lang_file[0] = '\0';
#ifdef ROCKBOX_NO_TEMP_SETTINGS_FILE
    assert(settings_load_config(CONFIGFILE, false));
#else
    assert(settings_load_config(CONFIGFILE ".new", false));
#endif
    dispatch(SHELL_SETTINGS_GET, "scroll delay");
    expect("\"value\":\"500\"");
    dispatch(SHELL_SETTINGS_GET, "lang");
    expect("\"value\":\"" LANG_DIR "/english.lng\"");

    puts("Settings tree, metadata, write validation and cfg round trips passed");
    exit(0);
}
