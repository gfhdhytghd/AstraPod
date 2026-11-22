#!/usr/bin/env python3
"""Adapt the stock Lua demos to the iPod's display and physical buttons."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
script = source.read_text()


def replace(before, after):
    global script
    if script.count(before) != 1:
        raise ValueError(f"{source}: expected one instance of {before!r}")
    script = script.replace(before, after)


def cut(start, end, body):
    global script
    begin = script.index(start)
    script = script[:begin] + body + script[script.index(end, begin):]


if source.stem == "helloworld":
    cut("-- Load a BMP file", "-- Flush the contents",
        """-- Draw an original sample image without relying on installed icon art.
local image = rb.new_image(24, 24)
for y = 1, 24 do
    for x = 1, 24 do
        image:set(x, y, rb.lcd_rgbpack(x * 10, y * 10, 160))
    end
end
draw_image(image)

""")
    replace('local pathname = rb.current_path() .. "test.txt"',
            'local pathname = rb.PLUGIN_DEMOS_DATA_DIR .. "/hello-lua.txt"')
    replace('local file = io.open(filename, "w+")',
            'local file = io.open(filename, "w")')
    replace('printf(file_get_contents(pathname))',
            'printf("%s", file_get_contents(pathname) or "Could not read the demo file")')
    replace("rb.contexts.CONTEXT_STD, rb.HZ", "rb.contexts.CONTEXT_WPS, rb.HZ")
    replace("until action == rb.actions.ACTION_STD_CANCEL",
            "until action == rb.actions.ACTION_WPS_MENU or action == rb.actions.ACTION_STD_CANCEL")
elif source.stem == "stopwatch":
    replace('require "buttons"\n', '')
    replace('/stopwatch.dat', '/stopwatch-lua.dat')
    cut("function LapsView:checkForScroll", "function LapsView:incTimer", "")
    cut("local Button = {", "-- Helper function\nfunction ticksToString", "")
    cut("function arrangeButtons", "LapsView:saveState()",
        """LapsView:init()
rb.lcd_putsxy(0, 10, "Play: start/stop")
rb.lcd_putsxy(0, 32, "Right: lap")
rb.lcd_putsxy(0, 54, "Left: reset")
rb.lcd_putsxy(0, 76, "Menu: quit")
repeat
    LapsView:incTimer()
    local action = rb.get_action(rb.contexts.CONTEXT_WPS, 2)
    if action == rb.actions.ACTION_WPS_PLAY then
        if LapsView.timer.counting then
            LapsView:stopTimer()
        else
            LapsView:startTimer()
        end
    elseif action == rb.actions.ACTION_WPS_SKIPNEXT then
        LapsView:newLap()
    elseif action == rb.actions.ACTION_WPS_SKIPPREV then
        LapsView:resetTimer()
    end
    LapsView:display()
    rb.lcd_update()
until action == rb.actions.ACTION_WPS_MENU or action == rb.actions.ACTION_STD_CANCEL

""")
    replace("        x = 80,", "        x = 135,")
    replace("        width = rb.LCD_WIDTH - 80,", "        width = rb.LCD_WIDTH - 135,")
    replace("    table.insert(self.lapTimes, self.timer.current)",
            """    if #self.lapTimes == 128 then table.remove(self.lapTimes, 1) end
    table.insert(self.lapTimes, self.timer.current)""")
    replace('        self.timer[v] = tonumber(fd:read("*line"))',
            '        self.timer[v] = math.max(0, tonumber(fd:read("*line")) or 0)')
    replace('    self.timer.counting = toboolean(fd:read("*line"))',
            """    fd:read("*line")
    self.timer.counting = false
    self.timer.prevTotal = self.timer.current""")
    replace('    while line do\n        table.insert(self.lapTimes, tonumber(line))',
            '''    while line and #self.lapTimes < 128 do
        local timestamp = tonumber(line)
        if timestamp and timestamp >= 0 then table.insert(self.lapTimes, timestamp) end''')

output.write_text(script)
