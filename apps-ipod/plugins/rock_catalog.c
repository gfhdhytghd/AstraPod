/* Installed plugin catalog. GNU GPL version 2 or later. */
#include "plugins/rock_catalog.h"
#include "rbpaths.h"
#include "system.h"

static const char *const paths[] = {
#include "rock-compat/rock_test_paths.h"
};

const char *rock_plugin_path(unsigned int index)
{
    return index < ARRAYLEN(paths) ? paths[index] : NULL;
}
