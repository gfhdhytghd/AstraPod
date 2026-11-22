/* Read-only Audioscrobbler viewer using native lists and detail dialogs. */
#include <limits.h>
#include "plugins/lastfm-log-compat.h"

struct lastfm_track {
    char *field[8];
    unsigned long time;
    unsigned long length;
};

static struct lastfm_log log_file = {.fd = -1};
static off_t *rows;
static char line[LASTFM_LINE_MAX];

static bool parse_track(char *text, struct lastfm_track *track)
{
    if (!text[0] || text[0] == '#')
        return false;
    char *field = text;
    for (int i = 0; i < 7; ++i)
    {
        track->field[i] = field;
        char *sep = rb->strchr(field, '\t');
        if (!sep)
        {
            if (i != 6)
                return false;
            field += rb->strlen(field);
            break;
        }
        *sep = '\0';
        field = sep + 1;
    }
    track->field[7] = field;
    char *duration = track->field[4];
    char *time = track->field[6];
    char *rating = track->field[5];
    return !rb->strchr(field, '\t') && track->field[2][0] &&
           (rating[0] == 'L' || rating[0] == 'S') && !rating[1] &&
           lastfm_number(&duration, '\0', &track->length) &&
           lastfm_number(&time, '\0', &track->time);
}

/* A cached read would change the scan position. Drawing runs only afterward. */
static bool load_track(int row, struct lastfm_track *track)
{
    off_t offset = rows[row];
    if (rb->lseek(log_file.fd, offset, SEEK_SET) != offset)
        return false;
    log_file.consumed = offset;
    log_file.next = log_file.end = 0;
    return lastfm_line(&log_file, line) == LASTFM_RECORD &&
           parse_track(line, track);
}

static const char *track_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    struct lastfm_track track;
    if (!load_track(row, &track))
        return "Unreadable play";
    rb->snprintf(buf, size, "%s - %s", track.field[2], track.field[0]);
    return buf;
}

static void track_details(int row)
{
    struct lastfm_track track;
    if (!load_track(row, &track))
        return;
    static char title[LASTFM_LINE_MAX];
    static char details[LASTFM_LINE_MAX + 128];
    rb->strlcpy(title, track.field[2], sizeof(title));
    rb->snprintf(details, sizeof(details),
                 "Artist: %s\nAlbum: %s\nDuration: %lu s\n%s\nTimestamp: %lu",
                 track.field[0], track.field[1], track.length,
                 track.field[5][0] == 'L' ? "Listened" : "Skipped", track.time);
    rb->view_text(title, details);
}

static int index_tracks(size_t capacity)
{
    size_t count = 0;
    for (;;)
    {
        if (rb->action_userabort(TIMEOUT_NOBLOCK))
            return -1;
        off_t offset = log_file.consumed;
        enum lastfm_read status = lastfm_line(&log_file, line);
        if (status == LASTFM_EOF)
            return count;
        if (status == LASTFM_ERROR)
            return -2;
        struct lastfm_track track;
        if (status == LASTFM_RECORD && parse_track(line, &track))
        {
            if (count == capacity)
                return -2;
            rows[count++] = offset;
        }
        rb->yield();
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    if (!path)
    {
        int selection = 0;
        MENUITEM_STRINGLIST(menu, "Last.fm log viewer", NULL,
                            "Open device log", "Open exported log", "Exit");
        int item = rb->do_menu(&menu, &selection, NULL, false);
        if (item != 0 && item != 1)
            return item == MENU_ATTACHED_USB ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = item ? LASTFM_EXPORT : LASTFM_LOG;
    }

    size_t bytes;
    rows = rb->plugin_get_buffer(&bytes);
    if (!rows || !lastfm_open(&log_file, path))
    {
        rb->splash(2 * HZ, "No Last.fm log");
        return PLUGIN_OK;
    }
    int count = index_tracks(MIN(bytes / sizeof(*rows), INT_MAX));
    if (count <= 0)
    {
        lastfm_close(&log_file);
        if (rb->plugin_usb_taken())
            return PLUGIN_USB_CONNECTED;
        if (count != -1)
            rb->splash(2 * HZ, count ? "Could not index Last.fm log" :
                                      "No valid plays");
        return PLUGIN_OK;
    }

    static struct gui_synclist list;
    rb->gui_synclist_init(&list, track_name, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&list, "Last.fm plays", Icon_Plugin);
    rb->gui_synclist_set_nb_items(&list, count);
    rb->gui_synclist_draw(&list);
    while (!rb->plugin_usb_taken())
    {
        int action = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (action == ACTION_STD_OK)
        {
            track_details(rb->gui_synclist_get_sel_pos(&list));
            rb->gui_synclist_draw(&list);
        }
        else if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    rb->gui_synclist_scroll_stop(&list);
    lastfm_close(&log_file);
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
}
