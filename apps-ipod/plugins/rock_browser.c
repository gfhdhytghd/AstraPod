/* Installed plugin categories. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "action.h"
#include "system.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "screens/browse/browser.h"
#include "speech/language.h"
#include "settings.h"
#include "lang.h"
#include "lang_enum.h"
#include "widgets/menu.h"

int rock_browser(void *unused)
{
    (void)unused;
    MENUITEM_STRINGLIST(categories, ID2P(LANG_PLUGINS), NULL,
                        ID2P(LANG_PLUGIN_APPS), ID2P(LANG_PLUGIN_GAMES),
                        ID2P(LANG_PLUGIN_DEMOS));
    static const char *const paths[] = {
        PLUGIN_APPS_DIR, PLUGIN_GAMES_DIR, PLUGIN_DEMOS_DIR
    };
    static const int titles[] = {
        LANG_PLUGIN_APPS, LANG_PLUGIN_GAMES, LANG_PLUGIN_DEMOS
    };
    int selected = 0;
    for (;;)
    {
        int choice = do_menu(&categories, &selected, NULL, false);
        if (choice < 0 || (unsigned)choice >= ARRAYLEN(paths))
            return choice == MENU_ATTACHED_USB ? GO_TO_ROOT : GO_TO_PREVIOUS;

        struct browse_context browse = {
            .dirfilter = SHOW_PLUGINS,
            .flags = BROWSE_NO_CONTEXT_MENU,
            .root = paths[choice],
            .title = (char *)str(titles[choice]),
            .icon = Icon_Plugin,
        };
        bool previous = action_set_menu_back(true);
        int result = rockbox_browse(&browse);
        action_set_menu_back(previous);
        if (result != GO_TO_PREVIOUS)
            return result;
    }
}
