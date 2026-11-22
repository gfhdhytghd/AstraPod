#!/usr/bin/env python3
"""Stage WAVView with the foreground arena and checked RIFF reader."""
from hashlib import sha256
from pathlib import Path
import sys

source, scanner, output = map(Path, sys.argv[1:])
data = source.read_bytes()
if sha256(data).hexdigest() != "a436f4c8142dbf73ec80c3af3a77f0c206c344d5dfa7325b845f8a5374065bcc":
    raise SystemExit(f"{source}: review the AstraPod WAVView adapter for this source")
upstream = data.decode()
header = upstream[:upstream.index("static struct wav_header")]
peaks = upstream[upstream.index("struct peakstruct"):upstream.index("/* global vars */")]
output.write_text(header + peaks + '''static char *audiobuf;
static size_t audiobuflen;
static uint32_t mempeakcount;
static uint32_t zoomlevel = 1;
static uint32_t center;

''' + scanner.read_text() + '''
enum plugin_status plugin_start(const void *parameter)
{
    if (!parameter)
    {
        rb->splash(HZ * 2, "Hold Select on a WAV file and choose View waveform");
        return PLUGIN_OK;
    }

    audiobuf = rb->plugin_get_buffer(&audiobuflen);
    if (!audiobuf)
        return PLUGIN_ERROR;

    enum wave_result result = readwavpeaks(parameter);
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (result == WAVE_INVALID)
        rb->splash(HZ * 2, "Requires a mono or stereo 16-bit PCM WAV");
    if (result != WAVE_READY)
        return PLUGIN_OK;

    center = mempeakcount / 2;
    displaypeaks();
    for (;;)
    {
        int action = rb->get_action(CONTEXT_KEYBOARD, TIMEOUT_BLOCK);
        uint32_t step = MAX(1u, mempeakcount / (zoomlevel * 10));
        switch (action)
        {
            case ACTION_KBD_UP:
                zoomlevel = MAX(1u, zoomlevel / 2);
                break;
            case ACTION_KBD_DOWN:
                if (zoomlevel < 32 && zoomlevel <= mempeakcount / 2)
                    zoomlevel *= 2;
                break;
            case ACTION_KBD_LEFT:
                center -= MIN(center, step);
                break;
            case ACTION_KBD_RIGHT:
                center = MIN(mempeakcount - 1, center + step);
                break;
            case ACTION_KBD_SELECT:
                break;
            case ACTION_KBD_DONE:
                rb->splash(HZ * 3, "Wheel: zoom   Left/Right: pan   Menu: exit");
                if (rb->plugin_usb_taken())
                    return PLUGIN_USB_CONNECTED;
                break;
            case ACTION_KBD_ABORT:
                return PLUGIN_OK;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;
                continue;
        }
        displaypeaks();
    }
}
''')
