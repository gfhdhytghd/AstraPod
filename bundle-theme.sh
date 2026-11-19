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
FONTS="$STAGE/.rockbox/fonts"
for name in 24x24-icons 24-spun-badges 24-seven-fifteen; do
    cp "$ROOT/apps-ipod/fonts/$name.fnt" "$FONTS/"
done
cp "$ROOT/apps-ipod/fonts/"LICENSE-*.txt "$FONTS/"
python3 "$ROOT/tools/fonts/compile.py" "$STAGE/generated"
cp "$STAGE/generated/"*.pbf "$FONTS/"
cat > "$FONTS/NOTICE-HarmonyOS-Sans.txt" <<'NOTICE'
Astra renders its bitmap text using HarmonyOS Sans and HarmonyOS Sans SC.
HarmonyOS Sans is Copyright 2021-2024 Huawei Device Co., Ltd.
Font terms: https://github.com/openharmony/global_system_resources/blob/master/LICENSE_Fonts
NOTICE

(cd "$STAGE" && zip -qr "$ZIP" .rockbox)
# Buildzip may still find data for the upstream plugin loader.
zip -qd "$ZIP" '.rockbox/rocks/*' '.rockbox/rocks/' '.rockbox/viewers.config' >/dev/null 2>&1 || true
echo "bundled native fonts and default-config.cfg into $ZIP"
