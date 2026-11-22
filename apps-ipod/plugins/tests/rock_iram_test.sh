#!/bin/sh
# Build the lease and the extent checks with host doubles. GPL v2+.
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
mkdir -p "$out/audio" "$out/usbstack"
cat > "$out/config.h" <<'HEADER'
#define ASTRAPOD_ROCK_COMPAT
#define IPOD_6G
#define USB_ENABLE_AUDIO
HEADER
printf 'unsigned int thread_self(void);\n' > "$out/kernel.h"
cat > "$out/system.h" <<'HEADER'
int disable_irq_save(void);
void restore_irq(int previous);
HEADER
cat > "$out/audio.h" <<'HEADER'
int audio_status(void);
HEADER
printf 'bool track_decode_busy(void);\n' > "$out/audio/track_decode.h"
: > "$out/pcm_sink.h"
cat > "$out/pcm.h" <<'HEADER'
#define PCM_SINK_BUILTIN 0
int pcm_current_sink(void);
HEADER
printf 'bool usb_inserted(void);\n' > "$out/usb.h"
cat > "$out/usbstack/usb_audio.h" <<'HEADER'
bool usb_audio_reserve_recording(void);
void usb_audio_release_recording(void);
HEADER
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined \
    -I"$out" -I"$root/apps-ipod" "$root/apps-ipod/plugins/rock_iram.c" \
    "$root/apps-ipod/plugins/tests/rock_iram_test.c" -o "$out/check"
"$out/check"
printf '%s\n' 'PASS IRAM extents, codec drain, competing owners and USB deferral'
