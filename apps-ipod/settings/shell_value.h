/***************************************************************************
 * Copyright (C) 2026 PodBox contributors
 * GNU General Public License (version 2+)
 *
 * PodLink Shell's view of a native setting and its accepted cfg values.
 ****************************************************************************/

#ifndef SHELL_VALUE_H
#define SHELL_VALUE_H

#include <stdbool.h>

struct menu_item_ex;
struct settings_list;

struct shell_value_info {
    const char *type;
    const char *allowed;
    const int *values;
    int count;
    bool has_range;
    int min, max, step;
    const char *unit;
    int decimals;
    int max_bytes;
    unsigned long choice_mask;
    bool has_choice_mask;
    const char *format;
};

void shell_value_info(const struct settings_list *setting,
                      struct shell_value_info *info);
bool shell_value_writable(const struct settings_list *setting,
                          const struct menu_item_ex *item);
const char *shell_value_set(const struct settings_list *setting,
                            const struct menu_item_ex *item, char *value);

#endif
