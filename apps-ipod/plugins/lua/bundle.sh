#!/bin/sh
# Install the stock games and the libraries loaded by AstraPod's Lua viewer.
set -eu
src=$1
games=$2/rocks/games
libs=$2/rocks/viewers/lua
demos=$2/rocks/demos
apps=$2/rocks/apps
adapter=$(dirname "$0")/stage-script.py
mkdir -p "$games/.picross" "$libs" "$demos" "$apps"
for name in boomshine picross pixel-painter; do
    cp "$src/$name.lua" "$games/"
done
cp "$src/picross/"*.picross "$games/.picross/"
for name in color draw_num draw_poly draw_floodfill lcd; do
    cp "$src/lua/include_lua/$name.lua" "$libs/"
done
for name in helloworld test_viewports; do
    python3 "$adapter" "$src/$name.lua" "$demos/$name.lua"
done
python3 "$adapter" "$src/stopwatch.lua" "$apps/stopwatch-lua.lua"
for name in print_buttons splashscroller; do
    python3 "$adapter" "$src/lua_scripts/$name.lua" "$demos/$name.lua"
done
cp "$src/lua_scripts/return2WPS.lua" "$apps/"
