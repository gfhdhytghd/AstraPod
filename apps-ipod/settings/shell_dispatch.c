/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Resolve PodLink Shell requests and serialize settings on the UI thread.
 ****************************************************************************/
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "podlink-shell-settings.h"
#include "settings/settings.h"
#include "settings/settings_list.h"
#include "settings/shell_menu.h"
#include "settings/shell_value.h"
#include "system/strutil.h"
#include "ui/menu_pane.h"

static char json[SHELL_SETTINGS_REPLY_SIZE];
static size_t used;

static void append(const char *format, ...)
{
    if (used >= sizeof(json))
        return;

    va_list args;
    va_start(args, format);
    int written = vsnprintf(json + used, sizeof(json) - used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(json) - used)
        used = sizeof(json);
    else
        used += written;
}

static void quoted_slice(const char *value, size_t length)
{
    append("\"");
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = value[i];
        if (c == '"' || c == '\\')
            append("\\%c", c);
        else if (c < 32)
            append("\\u%04x", c);
        else
            append("%c", c);
    }
    append("\"");
}

static void quoted(const char *value)
{
    quoted_slice(value, strlen(value));
}

static void named_string(const char *key, const char *value)
{
    append(",\"%s\":", key);
    quoted(value);
}

static void describe_setting(const struct shell_menu_row *row)
{
    const struct settings_list *setting = row->setting;
    struct shell_value_info info;
    char value[SETTINGS_MAX_LINE] = "";

    shell_value_info(setting, &info);
    cfg_to_string(setting, value, sizeof(value));
    named_string("cfg", setting->cfg_name);
    named_string("type", info.type);
    named_string("value", value);
    append(",\"writable\":%s", shell_value_writable(setting, row->item) ?
           "true" : "false");

    if (info.allowed)
    {
        append(",\"allowed\":[");
        const char *start = info.allowed;
        bool comma = false;
        for (unsigned i = 0; start; i++)
        {
            const char *end = strchr(start, ',');
            if (!info.has_choice_mask ||
                (i < sizeof(info.choice_mask) * 8 && (info.choice_mask & (1ul << i))))
            {
                if (comma)
                    append(",");
                quoted_slice(start, end ? (size_t)(end - start) : strlen(start));
                comma = true;
            }
            start = end ? end + 1 : NULL;
        }
        append("]");
    }

    if (info.count)
    {
        append(",\"%s\":[", info.allowed ? "values" : "allowed");
        for (int i = 0; i < info.count; i++)
            append("%s%d", i ? "," : "", info.values[i]);
        append("]");
    }

    if (info.has_range)
        append(",\"min\":%d,\"max\":%d,\"step\":%d",
               info.min, info.max, info.step);
    if (info.unit && *info.unit)
        named_string("unit", info.unit);
    if (info.decimals)
        append(",\"decimals\":%d", info.decimals);
    if (info.format)
        named_string("format", info.format);
    if (info.max_bytes)
        append(",\"max_bytes\":%d", info.max_bytes);
}

static void describe_row(const struct shell_menu_row *row)
{
    const char *kind = row->kind == SHELL_MENU ? "menu" :
                       row->kind == SHELL_SETTING ? "setting" : "action";
    append("{\"kind\":\"%s\"", kind);
    named_string("id", row->id);
    named_string("label", row->path[row->depth - 1]);
    append(",\"visible\":%s,\"available\":%s,\"path\":[",
           row->visible ? "true" : "false", row->available ? "true" : "false");
    for (unsigned i = 0; i < row->depth; i++)
    {
        if (i)
            append(",");
        quoted(row->path[i]);
    }
    append("]");
    if (row->setting && row->setting->cfg_name)
        describe_setting(row);
    append("}");
}

static void error_reply(const char *message)
{
    append("{\"error\":");
    quoted(message);
    append("}");
}

static void publish(void)
{
    if (used >= sizeof(json))
    {
        used = 0;
        error_reply("Settings reply is too large");
    }
    shell_settings_publish(json, used);
}

long shell_settings_dispatch(void)
{
    struct shell_menu_row row;
    char *name = shell_settings_body();
    char *value = NULL;
    unsigned operation = shell_settings_operation();
    used = 0;

    if (operation == SHELL_SETTINGS_LIST)
    {
        if (shell_menu_at(shell_settings_ordinal(), &row))
            describe_row(&row);
        else
            append("null");
    }
    else if (operation == SHELL_SETTINGS_ACTION)
    {
        if (name[0] != 1)
            error_reply("Settings action requires confirmation");
        else if (!shell_menu_find_action(name + 1, &row))
            error_reply("Settings action no longer matches the menu");
        else if (!shell_menu_action_allowed(&row))
            error_reply("Settings action is unavailable in the current menu");
        else if (!shell_menu_begin_action(&row))
            error_reply("The player refused to open this action");
        else
        {
            append("{\"status\":\"opened\"}");
            publish();
            return shell_menu_run_action(&row);
        }
    }
#if defined(HAVE_USBSTACK)
    else if (operation == SHELL_SETTINGS_GET && !strcmp(name, "pane-status"))
    {
        char status[640];
        menu_pane_status(status, sizeof(status));
        append("{\"kind\":\"setting\",\"cfg\":\"pane-status\","
               "\"id\":\"pane-status\",\"label\":\"Pane status\","
               "\"path\":[\"Pane status\"],\"type\":\"text\"");
        named_string("value", status);
        append(",\"writable\":false}");
    }
#endif
    else if (operation == SHELL_SETTINGS_SET &&
             !settings_parseline(name, &name, &value))
        error_reply("Expected cfg-name: value");
    else
    {
        const struct settings_list *setting = find_setting_by_cfgname(name);
        if (!setting || !shell_menu_find_setting(setting, &row))
            error_reply("Unknown menu setting");
        else if (value && !row.available)
            error_reply("Setting is unavailable in the current menu");
        else if (value)
        {
            const char *error = shell_value_set(setting, row.item, value);
            if (error)
                error_reply(error);
            else
            {
                shell_menu_find_setting(setting, &row);
                describe_row(&row);
            }
        }
        else
            describe_row(&row);
    }
    publish();
    return 0;
}
