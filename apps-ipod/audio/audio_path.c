/***************************************************************************
 * Original code from RockBox
 * was: apps/audio_path.c
 * Audio signal path management APIs
 *
 * Copyright (C) 2007 by Michael Sevakis
 * GNU General Public License (version 2+)
 *
 * Selects the audio input source. Vestigial on these targets, which have
 * no recording or line-in.
 ****************************************************************************/
#include "system.h"
#include "cpu.h"
#include "audio.h"
#ifdef HAVE_RECORDING
#include "button.h"
#endif
#include "general.h"
#include "settings/settings.h"

#if defined(IPOD_ACCESSORY_PROTOCOL) && defined(HAVE_LINE_REC)
#include "iap.h"
#endif

/* Selects the codec input and notifies a serial recording accessory. */
void audio_set_input_source(int source, unsigned flags)
{
#ifdef HAVE_RECORDING
    if (source == AUDIO_SRC_MIC && !mikey_supported())
        source = AUDIO_SRC_LINEIN;
#endif
#if defined(IPOD_ACCESSORY_PROTOCOL) && defined(HAVE_LINE_REC)
    static bool accessory_recording;
    bool line_in = source == AUDIO_SRC_LINEIN;
    if (accessory_recording != line_in)
        accessory_recording = iap_record(line_in);
#endif
    audio_input_mux(source, flags);
}

#if defined(HAVE_RECORDING) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
bool recording_capture_available(void)
{
    return true;
}
#endif
