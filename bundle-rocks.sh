#!/bin/sh
# Add the AstraPod module allowlist to the themed archive.
set -eu
root=$(cd "$(dirname "$0")" && pwd)
build=$(cd "${1:-.}" && pwd)
[ -d "$build/rock-compat/modules" ] || exit 0
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
while IFS= read -r module; do
    path=".rockbox/rocks/$module.rock"
    mkdir -p "$stage/$(dirname "$path")"
    cp "$build/rock-compat/modules/$module.rock" "$stage/$path"
done < "$root/apps-ipod/plugins/rock-modules.txt"
(cd "$stage" && zip -qr "$build/rockbox.zip" .rockbox)
