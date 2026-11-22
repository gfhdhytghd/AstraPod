/* Deferred native viewer dispatch. GNU GPL version 2 or later. */
#include "action.h"
#include "dir.h"
#include "files/filetypes.h"
#include "plugins/rock_native_handoff.h"
#include "plugins/rock_specialist.h"
#include "root_menu.h"
#include "screens/browse/browser.h"
#include "settings.h"
#include "string-extra.h"
#include "viewers/image_viewer/image_viewer_pub.h"
#include "viewers/text_viewer/text_viewer.h"

static int browse_target(const char *path)
{
    char root[MAX_PATH];
    size_t length = strlcpy(root, path, sizeof(root));
    if (length >= sizeof(root) - 1)
        return GO_TO_PREVIOUS;

    /* A trailing slash selects the directory itself in the Files browser. */
    if (length > 1 && root[length - 1] != '/' && dir_exists(root))
    {
        root[length++] = '/';
        root[length] = '\0';
    }

    unsigned depth = 0;
    for (const char *part = root + 1; *part; ++part)
        if (*part == '/' && ++depth >= MAX_DIR_LEVELS)
            return GO_TO_PREVIOUS;

    struct browse_context browse = {
        .dirfilter = SHOW_ALL,
        .flags = BROWSE_DIRFILTER,
        .root = root,
        .icon = Icon_Folder,
    };
    return rockbox_browse(&browse);
}

int rock_dispatch_native_handoff(const struct rock_handoff *handoff)
{
    int attr = filetype_get_attr(handoff->path) & FILE_ATTR_MASK;
    switch (handoff->kind)
    {
        case ROCK_HANDOFF_IMAGE:
            if (attr == FILE_ATTR_IMG)
                return image_viewer(handoff->path);
            break;
        case ROCK_HANDOFF_TEXT:
            if (attr == FILE_ATTR_TXT || attr == FILE_ATTR_BOOK ||
                attr == FILE_ATTR_LYRICS)
            {
                /* A document uses Menu to close and Left to page back. */
                bool previous = action_set_menu_back(false);
                int result = text_viewer(handoff->path);
                action_set_menu_back(previous);
                return result;
            }
            break;
        case ROCK_HANDOFF_BROWSE:
            return browse_target(handoff->path);
        case ROCK_HANDOFF_PLUGIN:
            break;
    }
    return GO_TO_PREVIOUS;
}
