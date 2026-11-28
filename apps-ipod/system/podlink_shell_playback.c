/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Queue playback commands on the UI thread and serve status over EP0.
 ****************************************************************************/
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "system.h"
#include "string-extra.h"
#include "audio.h"
#include "audio/playback.h"
#include "button.h"
#include "file.h"
#include "metadata.h"
#include "playlist/playlist.h"
#include "plugins/rock_compat.h"
#include "root_menu.h"
#include "podlink-shell-playback.h"
#include "sm1-test-control.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "viewers/lyric_viewer/lyric_viewer.h"

enum shell_playback_op {
    SHELL_PLAY_FILE = 1,
    SHELL_SEEK_MS,
    SHELL_NEXT,
    SHELL_PREV,
    SHELL_OPEN_LYRICS,
    SHELL_PROBE_VIDEO,
};

static unsigned operation;
static uint32_t position_ms;
static char path[MAX_PATH];
static unsigned result_sequence;
static unsigned result_operation;
static int result_error;

struct shell_playback_status {
    uint32_t version, sequence, operation;
    int32_t command_error;
    uint32_t state, elapsed_ms, length_ms, codec, frequency_hz, bits_per_sample;
    int32_t playback_error;
    char codec_name[24];
    char path[260];
};
_Static_assert(sizeof(struct shell_playback_status) == 328, "Playback wire size");

static void write_playback_status(uint8_t *buffer)
{
    struct audio_playback_debug snapshot;
    audio_playback_debug_snapshot(&snapshot);

    struct shell_playback_status reply = {
        .version = 1,
        .state = snapshot.state,
        .elapsed_ms = snapshot.elapsed_ms,
        .length_ms = snapshot.length_ms,
        .codec = snapshot.codec,
        .frequency_hz = snapshot.frequency_hz,
        .bits_per_sample = snapshot.bits_per_sample,
        .playback_error = snapshot.error,
    };
    int oldlevel = disable_irq_save();
    reply.sequence = result_sequence;
    reply.operation = result_operation;
    reply.command_error = result_error;
    restore_irq(oldlevel);
    const char *name = get_codec_string(snapshot.codec);
    if (name && snapshot.path[0])
        strlcpy(reply.codec_name, name, sizeof(reply.codec_name));
    strlcpy(reply.path, snapshot.path, sizeof(reply.path));
    memcpy(buffer, &reply, sizeof(reply));
}

static bool submit(unsigned op, const uint8_t *data, size_t size)
{
    if (op < SHELL_PLAY_FILE || op > SHELL_PROBE_VIDEO ||
        sm1_test_submitted != sm1_test_completed || sm1_test_record_active())
        return false;

    uint32_t ms = 0;
    if (op == SHELL_PLAY_FILE || op == SHELL_SEEK_MS)
    {
        if (size < 4)
            return false;
        ms = data[0] | (uint32_t)data[1] << 8 |
             (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        if (ms > LONG_MAX)
            return false;
        size -= 4;
        data += 4;
    }

    if (op == SHELL_PLAY_FILE || op == SHELL_PROBE_VIDEO)
    {
        if (size < 2 || size >= sizeof(path) || data[0] != '/' ||
            memchr(data, '\0', size) || memchr(data, '\r', size) ||
            memchr(data, '\n', size))
            return false;
        memcpy(path, data, size);
        path[size] = '\0';
    }
    else if (size)
        return false;

    operation = op;
    position_ms = ms;
    ++sm1_test_submitted;
    if (button_queue_try_post(SYS_SM1_TEST_CONTROL, SHELL_PLAYBACK_EVENT))
        return true;

    --sm1_test_submitted;
    return false;
}

void shell_playback_request(const struct usb_ctrlrequest *req,
                            uint8_t *buffer, size_t capacity, bool allowed)
{
    if (allowed && req->wLength <= capacity)
    {
        if (req->bRequestType == 0xc0 && req->wValue == 0 &&
            req->wLength == sizeof(struct shell_playback_status))
        {
            write_playback_status(buffer);
            usb_core_control_response(USB_CONTROL_ACK, buffer, req->wLength);
            return;
        }
        if (req->bRequestType == 0x40 &&
            submit(req->wValue, buffer, req->wLength))
        {
            usb_core_control_response(USB_CONTROL_ACK, NULL, 0);
            return;
        }
    }
    usb_core_control_response(USB_CONTROL_STALL, NULL, 0);
}

static int playback_dispatch(unsigned op)
{
    if (op == SHELL_PROBE_VIDEO)
    {
#ifdef HAVE_HW_H264
        return rock_probe_video(path) == 0 ? 0 : EIO;
#else
        return ENOSYS;
#endif
    }
    if (op == SHELL_PLAY_FILE)
    {
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return errno;
        close(fd);

        audio_stop();
        if (playlist_create(NULL, NULL) != 0 ||
            playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST,
                                  false, true) < 0)
            return EIO;
        playlist_start(0, position_ms, 0);
        return 0;
    }

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return ENODATA;

    if (op == SHELL_SEEK_MS)
    {
        struct audio_playback_debug snapshot;
        audio_playback_debug_snapshot(&snapshot);
        if (!snapshot.length_ms || position_ms >= snapshot.length_ms)
            return ERANGE;
        audio_seek_now(position_ms);
    }
    else if (op == SHELL_NEXT)
        audio_next();
    else if (op == SHELL_PREV)
        audio_prev();

    return 0;
}

long shell_playback_dispatch(void)
{
    unsigned op = operation;
    int error = playback_dispatch(op);

    int oldlevel = disable_irq_save();
    result_error = error;
    result_operation = op;
    result_sequence = ++sm1_test_completed;
    restore_irq(oldlevel);

    if (!error && op == SHELL_OPEN_LYRICS)
    {
        if (lyric_viewer() == GO_TO_ROOT)
            return SYS_USB_CONNECTED;
        button_queue_post(BUTTON_REDRAW, 0);
    }
    return 0;
}
