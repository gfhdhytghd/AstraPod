/* RockDoom foreground launcher. GNU GPL version 2 or later. */
#include "plugin.h"

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    if (rb->overlay_request("doom", NULL) < 0)
        return PLUGIN_ERROR;
    return PLUGIN_CHAIN_OVERLAY;
}
