/* Test the lifetime of overlay arguments. GNU GPL version 2 or later. */
#include "plugin.h"

enum plugin_status plugin_start(const void *parameter)
{
    size_t size = 0;
    void *space = rb->plugin_get_buffer(&size);
    if (!space || !size || !parameter ||
        rb->strcmp(parameter, "Copied from the launcher.") ||
        rb->overlay_request("probe", NULL) == 0)
        return PLUGIN_ERROR;
    return rb->view_text("Overlay probe", parameter);
}
