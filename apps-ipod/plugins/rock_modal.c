/* Modal widgets for foreground plugins. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "plugins/sdk/plugin.h"
#include "plugins/rock_widgets.h"
#include "widgets/color_picker.h"

struct rock_native_scope rock_modal_begin(void)
{
    rock_list_release(false);
    rock_flush_settings();
    return rock_display_native_begin();
}

void rock_modal_end_with_resume(const struct rock_native_scope *scope,
                                enum rock_display_resume resume)
{
    rock_list_release(false);
    rock_display_native_end(scope, resume);
    if (!rock_usb_taken())
        rock_begin_settings();
}

void rock_modal_end(const struct rock_native_scope *scope)
{
    rock_modal_end_with_resume(scope, ROCK_RESUME_PRESENT);
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

int rock_kbd_input_raw(char *buffer, int buflen)
{
    if (rock_usb_taken() || !buffer || buflen <= 0)
        return -1;

    struct rock_native_scope scope = rock_modal_begin();
    int result = dialog_input_raw(buffer, buflen);
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

bool rock_set_color(const char *title, unsigned *color, unsigned banned)
{
    if (rock_usb_taken())
        return true;

    struct rock_native_scope scope = rock_modal_begin();
    bool result = set_color(title, color, banned);
    rock_modal_end(&scope);
    return result || rock_usb_taken();
}
