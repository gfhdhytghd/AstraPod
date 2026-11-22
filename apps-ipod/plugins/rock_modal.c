/* Modal widgets for foreground plugins. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "plugins/sdk/plugin.h"
#include "plugins/rock_widgets.h"
#include "screens/browse/browser.h"

struct rock_native_scope rock_modal_begin(void)
{
    rock_list_release(false);
    rock_flush_settings();
    return rock_display_native_begin();
}

void rock_modal_end(const struct rock_native_scope *scope)
{
    rock_list_release(false);
    rock_display_native_end(scope, ROCK_RESUME_PRESENT);
    if (!rock_usb_taken())
        rock_begin_settings();
}

enum yesno_res rock_yesno(const struct text_message *message,
                          const struct text_message *yes,
                          const struct text_message *no)
{
    if (rock_usb_taken())
        return YESNO_USB;

    struct rock_native_scope scope = rock_modal_begin();
    enum yesno_res result = gui_syncyesno_run(message, yes, no);
    rock_modal_end(&scope);
    return result;
}

int rock_kbd_input(char *buffer, int buflen, ucschar_t *kbd)
{
    if (rock_usb_taken() || buflen <= 0)
        return -1;

    struct rock_native_scope scope = rock_modal_begin();
    int result = kbd_input(buffer, buflen, kbd);
    rock_modal_end(&scope);
    return result;
}

int rock_view_text(const char *title, const char *text)
{
    if (rock_usb_taken())
        return PLUGIN_USB_CONNECTED;

    struct rock_native_scope scope = rock_modal_begin();
    view_text(title, text);
    rock_modal_end(&scope);
    return rock_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}

static bool show_text_file(char *name, int attr, struct browser_context *browse)
{
    (void)browse;
    const char *ext = strrchr(name, '.');
    return (attr & ATTR_DIRECTORY) || (ext && !strcasecmp(ext, ".txt"));
}

int rock_pick_text_file(char *path, size_t size)
{
    if (rock_usb_taken() || !path || size < MAX_PATH)
        return -1;

    struct browse_context browse = {
        .dirfilter = SHOW_ALL,
        .flags = BROWSE_DIRFILTER | BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU,
        .callback_show_item = show_text_file,
        .title = "Select a text file",
        .icon = Icon_Plugin,
        .root = "/",
        .buf = path,
        .bufsize = size,
    };
    struct rock_native_scope scope = rock_modal_begin();
    bool previous = action_set_menu_back(true);
    rockbox_browse(&browse);
    action_set_menu_back(previous);
    rock_modal_end(&scope);
    return (browse.flags & BROWSE_SELECTED) && !rock_usb_taken() ? 0 : -1;
}
