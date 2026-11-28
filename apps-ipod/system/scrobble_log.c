/* AstraPod Audioscrobbler UTC log and legacy archive. GPL version 2 or later. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "file.h"
#include "logfile.h"
#include "system/scrobble_log.h"

static const char utc_header[] =
    "#AUDIOSCROBBLER/1.1\n#TZ/UTC\n"
    "#CLIENT/Rockbox " TARGET_NAME "\n"
    "#ARTIST\t#ALBUM\t#TITLE\t#TRACKNUM\t#LENGTH\t#RATING\t"
    "#TIMESTAMP\t#MUSICBRAINZ_TRACKID\n";

/* Move the old log before opening the UTC log. The audit trail survives a
 * failed append or power loss between the rename and new file creation. */
static bool archive_legacy(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT;

    char header[64];
    ssize_t bytes = read(fd, header, sizeof(header) - 1);
    close(fd);
    if (bytes <= 0)
        return bytes == 0;
    header[bytes] = 0;
    if (strstr(header, "\n#TZ/UTC\n") || strstr(header, "\n#TZ/UTC\r\n"))
        return true;

    char backup[sizeof(SCROBBLER_LEGACY_STEM) + 10];
    for (int part = 1; part <= 9999; part++)
    {
        snprintf(backup, sizeof(backup), SCROBBLER_LEGACY_STEM "_%04d.log",
                 part);
        fd = open(backup, O_RDONLY);
        if (fd < 0)
            return errno == ENOENT && rename(path, backup) == 0;
        close(fd);
    }
    return false;
}

int scrobble_log_open(void)
{
    if (!archive_legacy("/.scrobbler.1.log") ||
        !archive_legacy(SCROBBLER_LOG_PATH))
        return -1;
    return logfile_open_header(SCROBBLER_LOG_PATH, utc_header);
}
