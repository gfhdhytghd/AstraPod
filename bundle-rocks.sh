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
doom_launcher=".rockbox/rocks/games/doom.rock"
doom_overlay=".rockbox/rocks/overlays/doom.ovl"
if [ "${ROCK_DOOM:-0}" = 1 ]; then
    printf '%s\n' games/doom >> "$stage/manifest"
    mkdir -p "$stage/$(dirname "$doom_overlay")"
    cp "$build/rock-compat/modules/overlays/doom.ovl" "$stage/$doom_overlay"
fi
while IFS= read -r module; do
    path=".rockbox/rocks/$module.rock"
    mkdir -p "$stage/$(dirname "$path")"
    cp "$build/rock-compat/modules/$module.rock" "$stage/$path"
    if [ "$module" = games/snake2 ]; then
        cp "$root/apps/plugins/snake2.levels" "$stage/$(dirname "$path")/"
    fi
    if [ "$module" = games/sokoban ]; then
        cp "$root/apps/plugins/sokoban.levels" "$stage/$(dirname "$path")/"
    fi
    if [ "$module" = apps/disktidy ]; then
        cp "$root/apps/plugins/disktidy.config" "$stage/$(dirname "$path")/"
    fi
    if [ "$module" = viewers/lua ]; then
        sh "$root/apps-ipod/plugins/lua/bundle.sh" "$root/apps/plugins" "$stage/.rockbox"
    fi
    if [ "$module" = apps/dict ]; then
        python3 "$root/tools/rock-compat/wordnet.py" \
            "${WORDNET_ARCHIVE:-$HOME/.cache/podbox-wordnet/WordNet-3.0.tar.bz2}" \
            "$stage/$(dirname "$path")"
    fi
done < "$stage/manifest"
if grep -q "^games/sgt_puzzles/" "$stage/manifest"; then
    fonts="$stage/.rockbox/rocks/games/.sgt-fonts"
    mkdir -p "$fonts"
    unzip -oq "$root/apps/plugins/puzzles/fonts.zip" -d "$fonts"
    cp "$root/apps-ipod/plugins/tatham-fonts-LICENSE" "$fonts/LICENSE"
fi
(cd "$stage" && zip -qr "$build/rockbox.zip" .rockbox)
if [ "${ROCK_PITCH_DETECTOR:-0}" != 1 ] &&
        unzip -Z1 "$build/rockbox.zip" "$candidate_path" > /dev/null 2>&1; then
    zip -dq "$build/rockbox.zip" "$candidate_path"
fi
if [ "${ROCK_DOOM:-0}" != 1 ]; then
    for path in "$doom_launcher" "$doom_overlay"; do
        if unzip -Z1 "$build/rockbox.zip" "$path" > /dev/null 2>&1; then
            zip -dq "$build/rockbox.zip" "$path"
        fi
    done
fi
