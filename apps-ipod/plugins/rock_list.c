/* Retained list ownership for foreground plugins. GNU GPL version 2 or later. */
#include "plugins/rock_compat.h"
#include "plugins/rock_widgets.h"
#include "kernel.h"
#include "settings/settings.h"
#include "lang.h"

static struct gui_synclist *active;
static struct rock_native_scope list_scope;

bool rock_list_owns(const struct gui_synclist *list)
{
    return active == list;
}

void rock_list_release(bool present)
{
    if (!active)
        return;

    /* A plugin may return with a stack list. Forget it by address only. */
    gui_synclist_forget(active);
    active = NULL;
    rock_display_native_end(&list_scope, present ? ROCK_RESUME_PRESENT
                                                 : ROCK_RESUME_SILENT);
    rock_flush_settings();
    if (!rock_usb_taken())
        rock_begin_settings();
}

void rock_list_discard(void)
{
    if (!active)
        return;

    gui_synclist_forget(active);
    active = NULL;
    rock_display_native_end(&list_scope, ROCK_RESUME_DISCARD);
}

static void activate(struct gui_synclist *list)
{
    if (active == list)
        return;

    rock_list_release(false);
    rock_flush_settings();
    list_scope = rock_display_native_begin();
    active = list;
}

void rock_list_init(struct gui_synclist *list, list_get_name get_name,
                    void *data, bool scroll_all, int selected_size,
                    struct viewport parent[NB_SCREENS])
{
    if (active == list)
        rock_list_release(false);

    /* Resolve default geometry under native chrome without presenting a list. */
    struct rock_native_scope scope = rock_display_native_begin();
    gui_synclist_init(list, get_name, data, scroll_all, selected_size, parent);
    rock_display_native_end(&scope, ROCK_RESUME_SILENT);
}

void rock_list_draw(struct gui_synclist *list)
{
    if (rock_usb_taken())
        return;

    activate(list);
    gui_synclist_draw(list);
}

bool rock_list_button(struct gui_synclist *list, int *action)
{
    if (rock_usb_taken())
    {
        *action = SYS_USB_CONNECTED;
        return false;
    }
    if (IS_SYSEVENT(*action))
        return false;

    activate(list);
    return gui_synclist_do_button(list, action);
}

void rock_list_title(struct gui_synclist *list, const char *title,
                     enum themable_icons icon)
{
    title = P2STR((const unsigned char *)title);
    if (active == list)
        gui_synclist_set_title(list, title, icon);
    else
    {
        list->title = title;
        list->title_icon = icon;
    }
}

void rock_list_stop(struct gui_synclist *list)
{
    if (active == list)
        rock_list_release(true);
    else
        gui_synclist_scroll_stop(list);
}
