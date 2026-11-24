/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * PodLink access to the player's Settings rows.
 ****************************************************************************/
#ifndef SHELL_MENU_H
#define SHELL_MENU_H

#include <stdbool.h>

#define SHELL_MENU_DEPTH 12

struct menu_item_ex;
struct settings_list;

enum shell_menu_kind { SHELL_MENU, SHELL_SETTING, SHELL_ACTION };

struct shell_menu_row {
    const struct settings_list *setting;
    const struct menu_item_ex *item;
    enum shell_menu_kind kind;
    bool visible;
    bool available;
    unsigned depth;
    const char *path[SHELL_MENU_DEPTH];
    char label_buffer[128];
    char id[96];
};

bool shell_menu_at(unsigned ordinal, struct shell_menu_row *row);
bool shell_menu_find_setting(const struct settings_list *setting,
                             struct shell_menu_row *row);
bool shell_menu_find_action(const char *id, struct shell_menu_row *row);
bool shell_menu_action_allowed(const struct shell_menu_row *row);
bool shell_menu_begin_action(const struct shell_menu_row *row);
long shell_menu_run_action(const struct shell_menu_row *row);

#endif
