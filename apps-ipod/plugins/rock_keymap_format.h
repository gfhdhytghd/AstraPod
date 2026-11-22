/* Validate packed key remaps. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_KEYMAP_FORMAT_H
#define ASTRAPOD_ROCK_KEYMAP_FORMAT_H

#include "action.h"

#define ROCK_KEYMAP_MAX_ENTRIES 4096

static inline bool rock_keymap_sentinel(const struct button_mapping *entry)
{
    return (unsigned int)entry->action_code == CONTEXT_STOPSEARCHING &&
           entry->button_code == BUTTON_NONE &&
           entry->pre_button_code == BUTTON_NONE;
}

static inline bool rock_keymap_valid(const struct button_mapping *entries,
                                     int count)
{
    if (!entries || count <= 0 || count > ROCK_KEYMAP_MAX_ENTRIES)
        return false;

    int contexts = 0;
    while (contexts < count && !rock_keymap_sentinel(&entries[contexts]))
    {
        if (!(entries[contexts].action_code & CONTEXT_REMAPPED))
            return false;
        ++contexts;
    }
    if (contexts == count)
        return false;

    int first = contexts + 1;
    for (int i = 0; i < contexts; ++i)
    {
        int length = entries[i].pre_button_code;
        if (entries[i].button_code != first || length <= 0 ||
            length >= count - first)
            return false;

        int end = first + length;
        if (!rock_keymap_sentinel(&entries[end]))
            return false;
        for (int j = first; j < end; ++j)
        {
            if (entries[j].action_code < ACTION_NONE ||
                entries[j].action_code >= LAST_ACTION_PLACEHOLDER ||
                entries[j].button_code == BUTTON_NONE)
                return false;
        }
        first = end + 1;
    }
    return first == count;
}

#endif
