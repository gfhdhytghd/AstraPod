/* Export a snapshot of the native playback log. GNU GPL version 2 or later. */
#include <limits.h>
#include "system/library_files.h"
#include "plugins/lastfm-log-compat.h"

#define EXPORT_TEMP LASTFM_EXPORT ".tmp"
#undef UNTAGGED
#define UNTAGGED "<UNTAGGED>"
#define SCROBBLER_LOG_OK 1
#define SCROBBLER_LOG_NOMETADATA 2
#define SCROBBLER_LOG_SKIPTRACK 3
#define SCROBBLER_LOG_ERROR -1
#define MIN_ELAPSED_MS 500

static struct {
    int savepct;
    int minms;
    int tracknfo;
} gConfig = {50, MIN_ELAPSED_MS, 0};

static struct lastfm_log source = {.fd = -1};
static int output = -1;
static bool owns_temp;
static unsigned int entries, invalid;
static char record[LASTFM_LINE_MAX];

static const char *str_chk_valid(char *text, const char *fallback)
{
    if (!text || !*text)
        return fallback;
    for (char *p = text; *p; ++p)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    return text;
}

static unsigned long sbl_get_threshold(unsigned long length_ms)
{
    return (uint64_t)length_ms * gConfig.savepct / 100;
}

/* Include stock formatter here. */

static void discard(void *unused)
{
    (void)unused;
    lastfm_close(&source);
    if (output >= 0)
    {
        rb->close(output);
        output = -1;
    }
    if (owns_temp && !rb->remove(EXPORT_TEMP))
        owns_temp = false;
}

static int poll(void)
{
    int action = rb->get_action(CONTEXT_STD, TIMEOUT_NOBLOCK);
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        return PLUGIN_OK;
    if (rb->default_event_handler_ex(action, discard, NULL) == SYS_USB_CONNECTED)
        return PLUGIN_USB_CONNECTED;
    return 1;
}

static int export_log(void)
{
    for (;;)
    {
        int pending = poll();
        if (pending != 1)
            return pending;
        enum lastfm_read state = lastfm_line(&source, record);
        if (state == LASTFM_EOF)
            return 1;
        if (state == LASTFM_ERROR)
            return PLUGIN_ERROR;
        if (state == LASTFM_RECORD && (!record[0] || record[0] == '#'))
            continue;
        struct scrobbler_entry entry;
        char *path = record;
        if (state == LASTFM_INVALID ||
            !lastfm_number(&path, ':', &entry.timestamp) ||
            !lastfm_number(&path, ':', &entry.elapsed) ||
            !lastfm_number(&path, ':', &entry.length) ||
            path[0] != '/' || !path[1] || entry.length < 1000 ||
            entry.length > UINT32_MAX || entry.elapsed > entry.length)
        {
            invalid++;
            continue;
        }
        if (entry.elapsed < (unsigned int)gConfig.minms)
            continue;

        entry.path = path;
        int status = sbl_create_entry(&entry, output);
        if (status == SCROBBLER_LOG_ERROR)
            return PLUGIN_ERROR;
        if (status != SCROBBLER_LOG_SKIPTRACK)
            entries++;
        rb->splash_progress(0, 1, "Exporting %u plays. Menu cancels.", entries);
        rb->yield();
    }
}

static int export_history(void)
{
    static const char *lines[] = {"Write Last.fm export?",
                                  LASTFM_EXPORT};
    static const struct text_message message = {lines, ARRAYLEN(lines)};
    int answer = rb->gui_syncyesno_run(&message, NULL, NULL);
    if (answer != YESNO_YES)
        return answer == YESNO_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;

    rb->playback_log_flush();
    entries = invalid = 0;
    output = rb->open(EXPORT_TEMP, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (output < 0)
    {
        rb->splash(2 * HZ, "Could not create Last.fm export");
        return PLUGIN_OK;
    }
    owns_temp = true;
    int result = PLUGIN_ERROR;
    if (rb->fdprintf(output, "#AUDIOSCROBBLER/1.1\n#TZ/UNKNOWN\n"
                            "#CLIENT/AstraPod %s\n", rb->rbversion) < 0)
        goto done;
    rb->splash_progress_set_delay(HZ / 2);
    for (int index = 1; index <= 9999; ++index)
    {
        char path[MAX_PATH];
        rb->snprintf(path, sizeof(path), LIB_PLAYBACK_STEM "_%04d.log", index);
        if (!rb->file_exists(path))
            break;
        if (!lastfm_open(&source, path))
            goto done;
        result = export_log();
        lastfm_close(&source);
        if (result != 1)
            goto done;
        result = PLUGIN_ERROR;
    }
    if (rb->file_exists(LIB_PLAYBACK_LOG))
    {
        if (!lastfm_open(&source, LIB_PLAYBACK_LOG))
            goto done;
        result = export_log();
        lastfm_close(&source);
        if (result != 1)
            goto done;
    }
    if (!entries)
    {
        rb->splash(2 * HZ, "No matching plays");
        result = PLUGIN_OK;
        goto done;
    }
    int sync = rb->file_sync(output);
    int closed = rb->close(output);
    output = -1;
    result = sync || closed ? PLUGIN_ERROR : poll();
    if (result != 1)
        goto done;
    if (rb->rename(EXPORT_TEMP, LASTFM_EXPORT))
    {
        result = PLUGIN_ERROR;
        goto done;
    }
    owns_temp = false;
    rb->splashf(2 * HZ, "%u plays exported, %u invalid records",
                entries, invalid);
    result = PLUGIN_OK;

done:
    discard(NULL);
    if (result == PLUGIN_ERROR)
        rb->splash(2 * HZ, "Last.fm export failed");
    return result == PLUGIN_USB_CONNECTED ? result : PLUGIN_OK;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    int selection = 0;
    static const struct opt_items metadata[] = {
        {"Keep all", -1}, {"Skip untagged", -1}, {"Use filenames", -1},
    };
    MENUITEM_STRINGLIST(menu, "Last.fm exporter", NULL,
                        "Export playback log", "Listened threshold",
                        "Minimum elapsed", "Track information", "Exit");
    for (;;)
    {
        switch (rb->do_menu(&menu, &selection, NULL, false))
        {
            case 0:
                return export_history();
            case 1:
                rb->set_int("Listened threshold", "%", UNIT_PERCENT,
                            &gConfig.savepct, NULL, 10, 0, 100, NULL);
                break;
            case 2:
                rb->set_int("Minimum elapsed", "ms", UNIT_MS,
                            &gConfig.minms, NULL, 500, MIN_ELAPSED_MS, 10000,
                            NULL);
                break;
            case 3:
                rb->set_option("Track information", &gConfig.tracknfo,
                               RB_INT, metadata, ARRAYLEN(metadata), NULL);
                break;
            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;
            default:
                return PLUGIN_OK;
        }
    }
}
