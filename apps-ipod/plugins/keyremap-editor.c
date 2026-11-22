/* Edit the transport shortcuts in the native keymap. GPL-2.0-or-later. */
#include "plugin.h"
#include "input/core_keymap.h"
#include "plugins/rock_keymap_format.h"
#include "usb.h"

#ifdef BUTTON_RC_LEFT
#define TRANSPORT_KEYS 4
#else
#define TRANSPORT_KEYS 2
#endif
#define TRANSPORT_GROUPS (TRANSPORT_KEYS / 2)
#define MAP_ENTRIES (1 + 2 * TRANSPORT_GROUPS + TRANSPORT_KEYS)

static const struct button_mapping sentinel = LAST_ITEM_IN_LIST;
static const int transport_buttons[TRANSPORT_KEYS] = {
    BUTTON_LEFT, BUTTON_RIGHT,
#ifdef BUTTON_RC_LEFT
    BUTTON_RC_LEFT, BUTTON_RC_RIGHT,
#endif
};

static const int targets[] = {
    ACTION_NONE, ACTION_WPS_SKIPPREV, ACTION_WPS_SKIPNEXT,
    ACTION_WPS_PLAY, ACTION_WPS_VOLUP, ACTION_WPS_VOLDOWN,
};

static const struct opt_items action_names[] = {
    { "Default", -1 }, { "Previous track", -1 }, { "Next track", -1 },
    { "Play / Pause", -1 }, { "Volume up", -1 }, { "Volume down", -1 },
};

static const char *const key_names[TRANSPORT_KEYS] = {
    "iPod Left", "iPod Right",
#ifdef BUTTON_RC_LEFT
    "Remote Left", "Remote Right",
#endif
};

static unsigned int map_button(int index)
{
    return transport_buttons[index] | BUTTON_REL;
}

static int pack(struct button_mapping *map, const int *choices)
{
    int groups = 0;
    for (int group = 0; group < TRANSPORT_GROUPS; group++)
        groups += choices[2 * group] || choices[2 * group + 1];

    int context = 0;
    int next = groups + 1;
    map[groups] = sentinel;
    for (int group = 0; group < TRANSPORT_GROUPS; group++)
    {
        int first = next;
        for (int i = 2 * group; i < 2 * group + 2; i++)
        {
            if (choices[i])
                map[next++] = (struct button_mapping){
                    targets[choices[i]], map_button(i), transport_buttons[i],
                };
        }
        if (first == next)
            continue;

        int mask = group ? CONTEXT_REMOTE : 0;
        map[context++] = (struct button_mapping){
            CORE_CONTEXT_REMAP(CONTEXT_WPS | mask), first, next - first,
        };
        map[next++] = sentinel;
    }
    return next;
}

static bool unpack(const struct button_mapping *map, int count, int *choices)
{
    if (!rock_keymap_valid(map, count))
        return false;

    /* Accept only maps this editor could produce. Keep external .kmf maps
     * intact, including mappings for navigation and other screens. */
    for (int context = 0; !rock_keymap_sentinel(&map[context]); context++)
    {
        unsigned int code = map[context].action_code;
        int group = code == CORE_CONTEXT_REMAP(CONTEXT_WPS | CONTEXT_REMOTE);
        if (group >= TRANSPORT_GROUPS ||
            (!group && code != CORE_CONTEXT_REMAP(CONTEXT_WPS)))
            return false;

        int first = map[context].button_code;
        int end = first + map[context].pre_button_code;
        for (int entry = first; entry < end; entry++)
        {
            int index = 2 * group;
            if ((unsigned int)map[entry].button_code == map_button(index + 1))
                index++;

            if ((unsigned int)map[entry].button_code != map_button(index) ||
                map[entry].pre_button_code != transport_buttons[index] ||
                choices[index])
                return false;

            for (unsigned int action = 1; action < ARRAYLEN(targets); action++)
                if (map[entry].action_code == targets[action])
                    choices[index] = action;
            if (!choices[index])
                return false;
        }
    }

    struct button_mapping canonical[MAP_ENTRIES];
    return pack(canonical, choices) == count &&
           !rb->memcmp(map, canonical, count * sizeof(*map));
}

static bool load(int *choices)
{
    int fd = rb->open(CORE_KEYREMAP_FILE, O_RDONLY);
    if (fd < 0)
        return !rb->file_exists(CORE_KEYREMAP_FILE);

    struct button_mapping file[MAP_ENTRIES + 1];
    off_t bytes = rb->ffilesize(fd);
    bool ok = bytes >= (off_t)(2 * sizeof(*file)) &&
              bytes <= (off_t)sizeof(file) &&
              bytes % sizeof(*file) == 0 &&
              rb->read(fd, file, bytes) == bytes;
    rb->close(fd);
    int count = ok ? bytes / sizeof(*file) : 0;
    return count && file[0].action_code == KEYREMAP_VERSION &&
           file[0].button_code == KEYREMAP_HEADERID &&
           file[0].pre_button_code == count &&
           unpack(file + 1, count - 1, choices);
}

static bool save(const int *choices)
{
    static const char temp[] = CORE_KEYREMAP_FILE ".tmp";
    struct button_mapping file[MAP_ENTRIES + 1];
    int count = 1 + pack(file + 1, choices);
    file[0] = (struct button_mapping){ KEYREMAP_VERSION, KEYREMAP_HEADERID, count };
    size_t size = count * sizeof(*file);

    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return false;
    bool complete = rb->write(fd, file, size) == (ssize_t)size &&
                    rb->file_sync(fd) == 0;
    complete = rb->close(fd) == 0 && complete;
    if (complete && rb->rename(temp, CORE_KEYREMAP_FILE) == 0)
        return true;

    rb->remove(temp);
    return false;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    int choices[TRANSPORT_KEYS] = {0};
    if (!load(choices))
    {
        rb->view_text("Key Remap", "This .kmf file uses external mappings. "
                     "Back it up and remove it before editing.");
        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_ERROR;
    }

    MENUITEM_STRINGLIST(menu, "Key Remap", NULL, "iPod Left", "iPod Right",
#ifdef BUTTON_RC_LEFT
                        "Remote Left", "Remote Right",
#endif
                        "Restore defaults", "Save for next boot", "Quit");
    int selected = 0;
    while (true)
    {
        int row = rb->do_menu(&menu, &selected, NULL, false);
        if (row == MENU_ATTACHED_USB || rb->plugin_usb_taken())
            return PLUGIN_USB_CONNECTED;
        if (row < 0 || row == TRANSPORT_KEYS + 2)
            return PLUGIN_OK;
        if (row < TRANSPORT_KEYS)
        {
            if (rb->set_option(key_names[row], &choices[row], RB_INT,
                               action_names, ARRAYLEN(action_names), NULL))
                return PLUGIN_USB_CONNECTED;
        }
        else if (row == TRANSPORT_KEYS)
            rb->memset(choices, 0, sizeof(choices));
        else if (save(choices))
        {
            rb->splash(2 * HZ, "Saved. Restart to apply.");
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        }
        else
            rb->splash(2 * HZ, "Unable to save key map");
    }
}
