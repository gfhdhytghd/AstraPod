#!/bin/sh
# Install the stock games and the libraries loaded by AstraPod's Lua viewer.
set -eu
src=$1
games=$2/rocks/games
libs=$2/rocks/viewers/lua
mkdir -p "$games/.picross" "$libs"
for name in boomshine picross pixel-painter; do
    cp "$src/$name.lua" "$games/"
done
cp "$src/picross/"*.picross "$games/.picross/"
for name in color draw_num draw_poly draw_floodfill lcd; do
    cp "$src/lua/include_lua/$name.lua" "$libs/"
done
