/* AstraPod entry for the stock SDL Quake engine. GPL-2.0-or-later. */
#include "plugin.h"

enum plugin_status rock_quake_run(void);

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    return rock_quake_run();
}
