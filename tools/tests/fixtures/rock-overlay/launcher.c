/* Test the lifetime of overlay arguments. GNU GPL version 2 or later. */
#include "plugin.h"

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    char data[] = "Copied from the launcher.";
    if (rb->overlay_request("probe", data) < 0)
        return PLUGIN_ERROR;
    rb->memset(data, 'X', sizeof(data));
    return PLUGIN_CHAIN_OVERLAY;
}
