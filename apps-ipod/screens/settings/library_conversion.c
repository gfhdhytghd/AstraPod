/* Convert library entries in catalog order, retaining each completed output. */
#include "config.h"
#include <string.h>
#include "kernel.h"
#include "audio.h"
#include "metadata.h"
#include "powermgmt.h"
#include "lang.h"
#include "input/action.h"
#include "api/misc.h"
#include "widgets/splash.h"
#include "database/tagcache.h"
#include "playlist/playlist.h"
#include "audio/sm1_convert.h"
#include "screens/settings/library_conversion.h"

static volatile bool running;
static struct mp3entry metadata;
static char current_path[MAX_PATH];
static int checked, converted, deferred_event;
static long next_update;
static bool cancelled;

bool library_conversion_running(void)
{
    return running;
}

static bool poll_conversion(void)
{
    int action = get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (action == SYS_USB_CONNECTED || action == SYS_POWEROFF || action == SYS_REBOOT)
        deferred_event = action;
    if (deferred_event || action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        cancelled = true;
    if (!cancelled && !TIME_BEFORE(current_tick, next_update))
    {
        const char *name = strrchr(current_path, '/');
        splashf(0, str(LANG_LIBRARY_CONVERT_PROGRESS), checked, converted,
                name ? name + 1 : current_path);
        reset_poweroff_timer();
        next_update = current_tick + HZ;
    }
    return cancelled;
}

int library_conversion_run(void)
{
    struct tagcache_search search;
    int existing = 0, unsupported = 0, failed = 0;
    if (!tagcache_is_usable() || !tagcache_search_ready() ||
        !tagcache_search(&search, tag_filename))
    {
        splash(HZ * 2, ID2P(LANG_TAGCACHE_BUSY));
        return 0;
    }

    running = true;
    audio_stop();
    checked = converted = deferred_event = 0;
    next_update = current_tick;
    cancelled = false;
    while (tagcache_get_next(&search, current_path, sizeof(current_path)))
    {
        if (poll_conversion())
            break;
        ++checked;
        if (!get_metadata(&metadata, -1, current_path))
        {
            ++failed;
            continue;
        }
        if (metadata.frequency <= 48000)
            continue;
        switch (sm1_convert_track(current_path, 48000, poll_conversion))
        {
            case SM1_CONVERT_OK: ++converted; break;
            case SM1_CONVERT_EXISTS: ++existing; break;
            case SM1_CONVERT_UNSUPPORTED: ++unsupported; break;
            case SM1_CONVERT_CANCELLED: cancelled = true; break;
            default: ++failed; break;
        }
        if (cancelled)
            break;
        yield();
    }

    bool catalog_failed = search.failed;
    tagcache_search_finish(&search);
    if (converted || existing)
        playlist_retry_skipped();
    running = false;
    if (deferred_event)
        default_event_handler(deferred_event);
    else
    {
        if (catalog_failed)
            splash(2 * HZ, ID2P(LANG_TAGCACHE_BUSY));
        else if (cancelled)
            splash(HZ, ID2P(LANG_CANCEL));
        splashf(4 * HZ, str(LANG_LIBRARY_CONVERT_RESULTS),
                converted, existing, unsupported, failed);
    }
    return 0;
}
