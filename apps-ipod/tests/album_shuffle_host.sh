#!/bin/sh
# Build the playlist's ordering code with the host's POSIX file API.
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/file.h" <<'HEADER'
#include <unistd.h>
#define MAX_PATH 260
HEADER
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror \
    -I"$tmp" -Iapps-ipod -idirafter firmware/include \
    apps-ipod/tests/album_shuffle_host.c apps-ipod/playlist/album_shuffle.c \
    firmware/common/strnatcmp.c -o "$tmp/test"
"$tmp/test"
