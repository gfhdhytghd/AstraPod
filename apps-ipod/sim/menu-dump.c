/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * PODBOX_MENU_DUMP=<file>: before anything else runs, write every row of the
 * page menus to <file> and exit, for tools/tests/row_glyphs_test.py. One
 * line per row, tab-separated: the path of row indices from the root, the
 * root's name, the menu type, the label's lang id (-1 for a dynamic label),
 * the setting's cfg name (or -), the row's own glyph (0 for none) and the
 * icon its definition names. PODBOX_MENU_PIXELS=<file> boots the UI, walks
 * visible rows with their normal callbacks and appends the ink pixel count.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "audio/enc_config.h"
#include "lang.h"
#include "root_menu.h"
#include "system/activity.h"
#include "ui/native_theme_runtime.h"
#include "ui/scene.h"
#include "settings/settings.h"
#include "widgets/menu.h"
#include "menu-dump.h"

extern const struct menu_item_ex main_menu_, info_menu, album_covers_menu,
    lib_menu, moods_menu, journeys_menu, spike_menu, viewer_settings_menu,
    spun_menu, text_viewer_menu, playlist_options, tagcache_menu, file_menu,
    music_menu;
const struct menu_item_ex *fruit_menu_root(void);

static int label_id(const struct menu_item_ex *item)
{
    int type = item->flags & MENU_TYPE_MASK;
    if (item->flags & MENU_SETTING_LABEL)
        return P2ID(item->callback_and_desc->desc);
    if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        const struct settings_list *s = find_setting(item->variable);
        return s ? s->lang_id : -1;
    }
    if (item->flags & MENU_HAS_DESC)
        return P2ID(item->callback_and_desc->desc);
    return -1;
}

static void walk(FILE *out, const char *root, const struct menu_item_ex *menu,
                 char *path, int depth, bool pixels)
{
    int count = MENU_GET_COUNT(menu->flags);
    int type = menu->flags & MENU_TYPE_MASK;
    char *end = path + strlen(path);

    if (depth > 12)
        return;
    for (int i = 0; i < count; i++)
    {
        sprintf(end, "/%d", i);
        int ink = pixels ? menu_sim_row_ink(menu, i) : 0;
        if (ink == -1)
            continue;
        if (type == MT_RETURN_ID)
        {
            fprintf(out, "%s\t%s\tstring\t%d\t-\t%d\t-1\t%d\n", path, root,
                    (menu->flags & MENU_DYNAMIC_DESC) ? -1 :
                    P2ID((unsigned char *)menu->strings[i]),
                    menu_item_glyph(menu, i), ink);
            continue;
        }
        const struct menu_item_ex *item = menu->submenus[i];
        int itype = item->flags & MENU_TYPE_MASK;
        const struct settings_list *s =
            (itype == MT_SETTING || itype == MT_SETTING_W_TEXT) ?
            find_setting(item->variable) : NULL;
        int icon = (item->flags & MENU_HAS_DESC) ?
                   item->callback_and_desc->icon_id :
                   (item->flags & MENU_DYNAMIC_DESC) ?
                   item->menu_get_name_and_icon->icon_id : -1;
        fprintf(out, "%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\n", path, root, itype,
                label_id(item), s && s->cfg_name ? s->cfg_name : "-",
                menu_item_glyph(item, 0), icon, ink);
        if (itype == MT_MENU || itype == MT_RETURN_ID)
            walk(out, root, item, path, depth + 1, pixels);
        else if ((itype == MT_FUNCTION_CALL || itype == MT_FUNCTION_CALL_W_PARAM)
                 && (item->flags & MENU_HAS_DESC))
        {
            const struct menu_item_ex *(*child)(void) =
                itype == MT_FUNCTION_CALL ?
                item->function->settings.child_menu :
                item->function_param->settings.child_menu;
            const struct menu_item_ex *sub = child ? child() : NULL;
            if (sub)
                walk(out, root, sub, path, depth + 1, pixels);
        }
    }
    *end = '\0';
}

static void dump_roots(FILE *out, bool pixels)
{
    static const struct { const char *name; const struct menu_item_ex *menu; }
    roots[] = {
        { "settings", &main_menu_ }, { "system", &info_menu },
        { "album_covers", &album_covers_menu }, { "sound_library", &lib_menu },
        { "moods", &moods_menu }, { "journeys", &journeys_menu },
        { "spike", &spike_menu }, { "playlist_viewer", &viewer_settings_menu },
        { "spun", &spun_menu }, { "text_viewer", &text_viewer_menu },
        { "playlist_options", &playlist_options },
        { "database", &tagcache_menu }, { "files", &file_menu },
        { "music", &music_menu },
    };
    char path[128];
    if (pixels)
    {
        path[0] = '\0';
        walk(out, "home", &root_menu_, path, 0, true);
        push_activity_without_refresh(ACTIVITY_SETTINGS);
    }
    for (unsigned i = 0; i < sizeof(roots) / sizeof(*roots); i++)
    {
        path[0] = '\0';
        walk(out, roots[i].name, roots[i].menu, path, 0, pixels);
    }
    path[0] = '\0';
    walk(out, "stock", fruit_menu_root(), path, 0, pixels);
    if (pixels)
        pop_current_activity_without_refresh();
}

__attribute__((constructor)) static void menu_dump(void)
{
    const char *file = getenv("PODBOX_MENU_DUMP");
    if (!file)
        return;
    FILE *out = fopen(file, "w");
    if (!out)
        exit(1);
    dump_roots(out, false);
    exit(fclose(out) != 0);
}

void sim_menu_pixels(void)
{
    const char *file = getenv("PODBOX_MENU_PIXELS");
    if (!file)
        return;
    FILE *out = fopen(file, "w");
    if (!out)
        exit(1);

#ifdef HAVE_RECORDING
    /* This format exposes the bitrate page beneath Encoder Settings. */
    global_settings.rec_format = REC_FORMAT_MPA_L3;
#endif
    ui_scene_interrupt();
    const char *theme = global_settings.native_theme;
    if (strcmp(theme, "scrim"))
    {
        menu_paint(!strcmp(theme, "fruit-company") ? fruit_menu_root() :
                   &main_menu_, 0);
        if (!ui_theme_set("scrim", false))
            exit(1);
    }
    dump_roots(out, true);
    exit(fclose(out) != 0);
}
