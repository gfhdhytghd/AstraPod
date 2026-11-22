/* Foreground overlay handoff. GNU GPL version 2 or later. */
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_overlay.h"

#define ROCK_OVERLAY_NAME_MAX 32

static struct rock_overlay_launch pending;
static unsigned int foreground_id;
static bool accepting;
static bool requested;

void rock_overlay_begin(void)
{
    foreground_id = thread_self();
    requested = false;
    accepting = true;
}

static bool valid_name(const char *name)
{
    if (!name || !*name)
        return false;
    for (size_t i = 0; name[i]; ++i)
    {
        unsigned char c = name[i];
        if (i == ROCK_OVERLAY_NAME_MAX ||
            !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              (i && c == '_')))
            return false;
    }
    return true;
}

int rock_overlay_request(const char *name, const char *parameter)
{
    if (!accepting || thread_self() != foreground_id || requested ||
        rock_usb_taken() || !valid_name(name))
        return -1;

    size_t length = 0;
    if (parameter)
        while (length < sizeof(pending.parameter) && parameter[length])
            ++length;
    if (length == sizeof(pending.parameter))
        return -1;

    int written = snprintf(pending.path, sizeof(pending.path), "%s/%s%s",
                           ROCK_OVERLAY_DIR, name, ROCK_OVERLAY_SUFFIX);
    if (written < 0 || (size_t)written >= sizeof(pending.path))
        return -1;

    pending.has_parameter = parameter != NULL;
    if (parameter)
        memcpy(pending.parameter, parameter, length + 1);
    else
        pending.parameter[0] = '\0';
    requested = true;
    return 0;
}

bool rock_overlay_take(struct rock_overlay_launch *launch)
{
    if (!accepting || thread_self() != foreground_id)
        return false;
    accepting = false;
    if (requested)
        *launch = pending;
    return requested;
}

void rock_overlay_end(void)
{
    accepting = false;
    requested = false;
    memset(&pending, 0, sizeof(pending));
}
