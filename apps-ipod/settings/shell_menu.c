/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Walk Settings in display order and invoke native action rows.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "input/action.h"
#include "kernel.h"
#include "lang.h"
#include "version.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "settings/shell_menu.h"
#include "system/activity.h"
#include "widgets/menu.h"

extern const struct menu_item_ex main_menu_;

struct search {
    struct shell_menu_row *row;
    const struct settings_list *setting;
    const char *id;
    unsigned remaining;
    unsigned indices[SHELL_MENU_DEPTH];
    const struct menu_item_ex *items[SHELL_MENU_DEPTH];
};

static const struct menu_function_settings *function_settings(
    const struct menu_item_ex *item)
{
    switch (item->flags & MENU_TYPE_MASK)
    {
        case MT_FUNCTION_CALL:
            return &item->function->settings;
        case MT_FUNCTION_CALL_W_PARAM:
            return &item->function_param->settings;
        default:
            return NULL;
    }
}

static const struct settings_list *menu_setting(const void *variable)
{
    for (int i = 0; i < nb_settings; i++)
    {
        if (settings[i].setting == variable && settings[i].cfg_name &&
            !(settings[i].flags & F_DEPRECATED))
            return &settings[i];
    }
    return NULL;
}

static const char *row_label(const struct menu_item_ex *item,
                             const struct settings_list *setting,
                             unsigned index, char *buf, size_t len)
{
    int type = item->flags & MENU_TYPE_MASK;

    if ((type == MT_SETTING || type == MT_SETTING_W_TEXT) &&
        !(item->flags & MENU_SETTING_LABEL))
        return !setting ? "Not Done yet!" : setting->lang_id < 0 ?
               setting->cfg_name : (const char *)str(setting->lang_id);
    if (item->flags & MENU_DYNAMIC_DESC)
    {
        const struct menu_get_name_and_icon *desc =
            item->menu_get_name_and_icon;
        return desc->list_get_name(index, desc->list_get_name_data, buf, len);
    }
    if (item->flags & MENU_HAS_DESC)
        return P2STR(item->callback_and_desc->desc);
    return "";
}

static uint32_t id_hash(uint32_t hash, const char *text)
{
    /* Include field separators so different path splits hash differently. */
    do
    {
        hash ^= (unsigned char)*text;
        hash *= 16777619u;
    } while (*text++);
    return hash;
}

static bool select_row(struct search *search, const void *variable,
                       enum shell_menu_kind kind, unsigned depth)
{
    struct shell_menu_row *row = search->row;
    size_t pos = 0;

    for (unsigned i = 0; i < depth; i++)
        pos += snprintf(row->id + pos, sizeof(row->id) - pos, "/%u",
                        search->indices[i]);
    if (search->id && (strncmp(search->id, row->id, pos) ||
                       search->id[pos] != ':'))
        return false;

    const struct settings_list *setting = variable ? menu_setting(variable) : NULL;
    if (search->setting && setting != search->setting)
        return false;

    /* Only the selected path runs callbacks. Some inspect files on disk. */
    uint32_t hash = id_hash(2166136261u, rbversion);
    for (unsigned i = 0; i < depth; i++)
    {
        /* The native Settings tree uses dynamic descriptions on leaves. */
        row->path[i] = row_label(search->items[i], i + 1 == depth ? setting : NULL,
                                 search->indices[i], row->label_buffer,
                                 sizeof(row->label_buffer));
        hash = id_hash(hash, row->path[i]);
    }
    snprintf(row->id + pos, sizeof(row->id) - pos, ":%08lx",
             (unsigned long)hash);
    if (search->id && strcmp(search->id, row->id))
        return false;

    row->depth = depth;
    row->item = search->items[depth - 1];
    row->setting = setting;
    row->kind = kind;
    row->available = true;
    row->visible = true;
    for (unsigned i = 1; i < depth; i++)
    {
        const struct menu_item_ex *item = search->items[i];
        row->available = row->available &&
                        menu_item_event(ACTION_REQUEST_MENUITEM, item) != ACTION_EXIT_MENUITEM;
        row->visible = row->visible && row->available && menu_item_in_settings_mode(item);
    }
    return true;
}

static bool walk(struct search *search, const struct menu_item_ex *item,
                 unsigned index, unsigned depth)
{
    if (!item || depth == SHELL_MENU_DEPTH)
        return false;

    unsigned type = item->flags & MENU_TYPE_MASK;
    const struct menu_function_settings *metadata = function_settings(item);
    const void *variable = NULL;
    const struct menu_item_ex *children = NULL;
    enum shell_menu_kind kind;

    if (metadata)
    {
        kind = SHELL_ACTION;
        variable = metadata->variable;
        if (metadata->child_menu)
            children = metadata->child_menu();
    }
    else if (type == MT_SETTING || type == MT_SETTING_W_TEXT)
    {
        kind = SHELL_SETTING;
        variable = item->variable;
    }
    else if (type == MT_MENU || type == MT_RETURN_ID)
    {
        kind = SHELL_MENU;
        if (type == MT_MENU)
            children = item;
    }
    else
        return false;

    search->indices[depth] = index;
    search->items[depth] = item;

    bool match = search->setting ? variable == search->setting->setting :
                 search->id ? kind == SHELL_ACTION : search->remaining-- == 0;
    if (match && select_row(search, variable, kind, depth + 1))
        return true;

    if (children)
    {
        unsigned count = MENU_GET_COUNT(children->flags);
        for (unsigned i = 0; i < count; i++)
            if (walk(search, children->submenus[i], i, depth + 1))
                return true;
    }
    return false;
}

bool shell_menu_at(unsigned ordinal, struct shell_menu_row *row)
{
    struct search search = {.row = row, .remaining = ordinal};
    return walk(&search, &main_menu_, 0, 0);
}

bool shell_menu_find_setting(const struct settings_list *setting,
                             struct shell_menu_row *row)
{
    struct search search = {.row = row, .setting = setting};
    return setting && walk(&search, &main_menu_, 0, 0);
}

bool shell_menu_find_action(const char *id, struct shell_menu_row *row)
{
    struct search search = {.row = row, .id = id};
    return walk(&search, &main_menu_, 0, 0);
}

bool shell_menu_action_allowed(const struct shell_menu_row *row)
{
    return row->kind == SHELL_ACTION && row->available;
}

bool shell_menu_begin_action(const struct shell_menu_row *row)
{
    return menu_item_event(ACTION_ENTER_MENUITEM, row->item) != ACTION_EXIT_MENUITEM;
}

long shell_menu_run_action(const struct shell_menu_row *row)
{
    const struct menu_item_ex *item = row->item;
    enum current_activity activity = get_current_activity();
    int result;

    if ((item->flags & MENU_TYPE_MASK) == MT_FUNCTION_CALL_W_PARAM)
        result = item->function_param->function_w_param(
                     item->function_param->param);
    else
        result = item->function->function();

    if (action_menu_back() && get_current_activity() != activity)
        push_current_activity(activity);

    menu_item_event(ACTION_EXIT_MENUITEM, item);
    /* Menu navigation codes belong to the action's own menu. */
    return (item->flags & MENU_FUNC_CHECK_RETVAL) &&
           result == MENU_ATTACHED_USB ? SYS_USB_CONNECTED : 0;
}
