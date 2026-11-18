#!/bin/sh
# Bundle PodBox's native fonts and initial configuration into a built ZIP.
# Usage: run from inside a build dir, or pass the build dir as an argument.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$(cd "${1:-.}" && pwd)"
ZIP="$BUILDDIR/rockbox.zip"

if [ ! -f "$ZIP" ]; then
    echo "bundle-theme.sh: no rockbox.zip in $BUILDDIR" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/.rockbox/fonts"
cp "$ROOT/apps-ipod/default-config.cfg" "$STAGE/.rockbox/"
cp "$ROOT/apps-ipod/fonts/"*.fnt "$ROOT/apps-ipod/fonts/"LICENSE-*.txt "$STAGE/.rockbox/fonts/"

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)
# Buildzip may still find data for the upstream plugin loader.
zip -qd "$ZIP" '.rockbox/rocks/*' '.rockbox/rocks/' '.rockbox/viewers.config' >/dev/null 2>&1 || true
echo "bundled native fonts and default-config.cfg into $ZIP"
