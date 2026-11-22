#!/bin/sh
# Install the allowlisted Lua entries and their helpers.
set -eu
src=$1
rocks=$2/rocks
adapter=$(dirname "$0")
libs=$rocks/viewers/lua
mkdir -p "$rocks/games/.picross" "$libs" "$rocks/demos" "$rocks/apps"
cp "$src/picross/"*.picross "$rocks/games/.picross/"
for name in color draw_num draw_poly draw_floodfill lcd blit draw draw_text \
            image image_save math_ex menubuttons strftime timer; do
    python3 "$adapter/stage-script.py" "$src/lua/include_lua/$name.lua" "$libs/$name.lua"
done
cp "$adapter/common.lua" "$libs/"
sed -n 's/^LUA_ENTRY(\([^,]*\), \([^,]*\), .*)$/\1 \2/p' "$adapter/entries.def" |
while read -r category name; do
    out=$rocks/$category/$name.lua
    if [ -f "$adapter/scripts/$name.lua" ]; then
        cp "$adapter/scripts/$name.lua" "$out"
    elif [ "$category" = games ]; then
        cp "$src/$name.lua" "$out"
    elif [ "$name" = stopwatch-lua ]; then
        python3 "$adapter/stage-script.py" "$src/stopwatch.lua" "$out"
    elif [ -f "$src/lua_scripts/$name.lua" ]; then
        python3 "$adapter/stage-script.py" "$src/lua_scripts/$name.lua" "$out"
    else
        python3 "$adapter/stage-script.py" "$src/$name.lua" "$out"
    fi
done
