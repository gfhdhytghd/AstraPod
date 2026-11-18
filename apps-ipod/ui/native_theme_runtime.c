/* PodBox native theme loading and Settings UI. GPL version 2+. */
#include "config.h"
#include "core_alloc.h"
#include "dir.h"
#include "file.h"
#include "font.h"
#include "lang.h"
#include "logfile.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "draw/viewport.h"
#include "lcd.h"
#include "metadata/album_palette.h"
#include "settings/settings.h"
#include "widgets/list.h"
#include "native_theme_runtime.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define NATIVE_THEME_DIR ROCKBOX_DIR "/native-themes"
#define NATIVE_THEME_EXT ".podtheme"

static int current_theme;
static unsigned generation;

static bool valid_name(const char *name, size_t len)
{
    if (!len || len >= UI_THEME_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; ++i)
    {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

static bool builtin_name(const char *name)
{
    return !strcmp(name, "scrim") || !strcmp(name, "light");
}

static bool theme_from_file(struct ui_theme *theme, const char *name)
{
    char path[sizeof(NATIVE_THEME_DIR NATIVE_THEME_EXT) + UI_THEME_NAME_MAX];
    snprintf(path, sizeof(path), NATIVE_THEME_DIR "/%s" NATIVE_THEME_EXT, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    off_t size = ffilesize(fd);
    bool ok = false;
    if (size > 0 && size <= UI_THEME_FILE_MAX)
    {
        int handle = core_alloc_free_space(size);
        if (handle > 0)
        {
            char *data = core_get_data_pinned(handle);
            ok = read(fd, data, size) == size && ui_theme_parse(theme, data, size);
            core_put_data_pinned(data);
            core_free(handle);
        }
    }
    close(fd);
    return ok;
}

static void theme_font_path(char *path, size_t size, const char *name)
{
    snprintf(path, size, FONT_DIR "/%s.fnt", name);
}

static bool theme_has_fonts(const struct ui_theme *theme)
{
    char path[sizeof(FONT_DIR "/.fnt") + UI_THEME_FONT_NAME_MAX];
    for (int i = 0; i < UI_FONT_COUNT; ++i)
    {
        theme_font_path(path, sizeof(path), theme->font[i]);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return false;
        char magic[4];
        bool valid = ffilesize(fd) >= 36 &&
                     read(fd, magic, sizeof(magic)) == sizeof(magic) &&
                     !memcmp(magic, "RB12", sizeof(magic));
        close(fd);
        if (!valid)
            return false;
    }
    return true;
}

static bool select_theme(const char *name)
{
    bool name_ok = valid_name(name, strlen(name));
    bool valid = false;
    int replacement = name_ok ? core_alloc_free_space(sizeof(struct ui_theme)) : 0;
    if (replacement > 0)
    {
        struct ui_theme *tokens = core_get_data_pinned(replacement);
        if (builtin_name(name))
        {
            ui_theme_builtin(tokens, !strcmp(name, "light"));
            valid = true;
        }
        else
            valid = theme_from_file(tokens, name) && theme_has_fonts(tokens);
        core_put_data_pinned(tokens);
        if (!valid)
            replacement = core_free(replacement);
    }
    if (current_theme > 0)
        core_free(current_theme);
    current_theme = replacement > 0 ? replacement : 0;
    ++generation;

    if (!valid)
    {
        int fd = logfile_open(ROCKBOX_DIR "/native-theme.log");
        if (fd >= 0)
        {
            logfile_printf(fd, "Native theme rejected, using Scrim (%s)\n",
                           name_ok ? name : "invalid name");
            logfile_close(fd);
        }
    }
    return valid;
}

void ui_theme_apply_settings(void)
{
    select_theme(global_settings.native_theme);
}

void ui_theme_snapshot(struct ui_theme *out)
{
    if (current_theme > 0)
    {
        struct ui_theme *theme = core_get_data_pinned(current_theme);
        *out = *theme;
        core_put_data_pinned(theme);
    }
    else
        ui_theme_builtin(out, false);
}

unsigned ui_theme_generation(void)
{
    return generation;
}

unsigned ui_theme_rgb565(enum ui_color_role role)
{
    struct ui_album_palette palette;
    album_palette_snapshot(&palette);

    uint32_t rgb;
    if (current_theme > 0)
    {
        struct ui_theme *theme = core_get_data_pinned(current_theme);
        rgb = ui_theme_color(theme, role, &palette);
        core_put_data_pinned(theme);
    }
    else
    {
        struct ui_theme theme;
        ui_theme_builtin(&theme, false);
        rgb = ui_theme_color(&theme, role, &palette);
    }
    return LCD_RGBPACK(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

unsigned ui_theme_duration(unsigned frames)
{
    if (!global_settings.animations)
        return 0;
    if (current_theme <= 0)
        return frames;

    struct ui_theme *theme = core_get_data_pinned(current_theme);
    unsigned scale = theme->motion_scale;
    core_put_data_pinned(theme);
    return ((uint64_t)frames * scale + UI_THEME_MOTION_ONE / 2) / UI_THEME_MOTION_ONE;
}

int ui_theme_font_load(const struct ui_theme *theme, enum ui_font_role role, int glyphs)
{
    char path[sizeof(FONT_DIR "/.fnt") + UI_THEME_FONT_NAME_MAX];
    theme_font_path(path, sizeof(path), theme->font[role]);
    return font_load_ex(path, 0, glyphs);
}

static size_t file_theme_name(const char *file)
{
    size_t len = strlen(file), suffix = sizeof(NATIVE_THEME_EXT) - 1;
    if (len <= suffix || strcmp(file + len - suffix, NATIVE_THEME_EXT))
        return 0;
    len -= suffix;
    return valid_name(file, len) ? len : 0;
}

static int find_themes(char (*names)[UI_THEME_NAME_MAX], int capacity)
{
    DIR *dir = opendir(NATIVE_THEME_DIR);
    if (!dir)
        return 0;
    struct dirent *entry;
    int count = 0;
    while (count < capacity && (entry = readdir(dir)) != NULL)
    {
        size_t len = file_theme_name(entry->d_name);
        if (!len || (dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) ||
            (len == 5 && (!memcmp(entry->d_name, "scrim", len) ||
                                  !memcmp(entry->d_name, "light", len))))
            continue;
        if (names)
        {
            memcpy(names[count], entry->d_name, len);
            names[count][len] = '\0';
        }
        ++count;
    }
    closedir(dir);
    return count;
}

static const char *theme_name(int item, void *data, char *buf, size_t len)
{
    (void)buf;
    (void)len;
    char (*files)[UI_THEME_NAME_MAX] = data;
    static const char *const builtins[] = { "Scrim", "Light" };
    return item < 2 ? builtins[item] : files[item - 2];
}

int ui_theme_choose(void)
{
    int count = find_themes(NULL, INT_MAX / UI_THEME_NAME_MAX - 2);
    int names_handle = count ? core_alloc_free_space(count * UI_THEME_NAME_MAX) : 0;
    char (*files)[UI_THEME_NAME_MAX] = names_handle > 0 ? core_get_data_pinned(names_handle) : NULL;
    count = files ? find_themes(files, count) : 0;

    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_NATIVE_THEME), count + 2, files);
    info.get_name = theme_name;
    bool to_root = simplelist_show_list(&info);
    if (!to_root && info.selection >= 0)
    {
        const char *name = info.selection == 0 ? "scrim" : info.selection == 1 ?
                           "light" : files[info.selection - 2];
        bool applied = select_theme(name);
        snprintf(global_settings.native_theme, sizeof(global_settings.native_theme),
                 "%s", applied ? name : "scrim");
        settings_apply_display_colors();
        viewportmanager_theme_changed(THEME_STATUSBAR | THEME_UI_VIEWPORT);
        settings_save();
    }
    if (files)
    {
        core_put_data_pinned(files);
        core_free(names_handle);
    }
    return to_root ? GO_TO_ROOT : 0;
}
