#!/bin/sh
# Add the AstraPod module allowlist to the themed archive.
set -eu
root=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:-.}" && pwd)
[ -d "$build/rock-compat/modules" ] || exit 0
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp "$root/apps-ipod/plugins/rock-modules.txt" "$stage/manifest"
candidate=apps/pitch_detector
candidate_path=".rockbox/rocks/$candidate.rock"
if [ "${ROCK_PITCH_DETECTOR:-0}" = 1 ]; then
    printf '%s\n' "$candidate" >> "$stage/manifest"
fi
while IFS= read -r module; do
    path=".rockbox/rocks/$module.rock"
    mkdir -p "$stage/$(dirname "$path")"
    cp "$build/rock-compat/modules/$module.rock" "$stage/$path"
    if [ "$module" = games/snake2 ]; then
        cp "$root/apps/plugins/snake2.levels" "$stage/$(dirname "$path")/"
    fi
done < "$stage/manifest"
(cd "$stage" && zip -qr "$build/rockbox.zip" .rockbox)
if [ "${ROCK_PITCH_DETECTOR:-0}" != 1 ] &&
        unzip -Z1 "$build/rockbox.zip" "$candidate_path" > /dev/null 2>&1; then
    zip -dq "$build/rockbox.zip" "$candidate_path"
fi
