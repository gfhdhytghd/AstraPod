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

elif source.stem == "splashscroller":
    replace("rb.splash_scroller(rb.HZ * 5, ",
            'rb.view_text("Splash Scroller", ')
elif source.stem == "print_buttons":
    replace('rb.splash_scroller(rb.HZ, table.concat(s, "\\n"))',
            'rb.view_text("Buttons Found", table.concat(s, "\\n"))\n'
            'rb.button_clear_queue()')
    replace('    rb.splash_scroller(-2, table.concat(s_t, "\\n"))',
            '''    rb.lcd_clear_display()
    for row = 1, math.min(#s_t, max_lines) do
        rb.lcd_putsxy(0, (row - 1) * h, s_t[row])
    end
    rb.lcd_update()''')
    replace('until keyrpt >= 2',
            '''until keyrpt >= 2 or lastkey == rb.buttons.BUTTON_MENU + rb.buttons.BUTTON_REL
                  or (rb.buttons.BUTTON_RC_MENU and
                      lastkey == rb.buttons.BUTTON_RC_MENU + rb.buttons.BUTTON_REL)''')

elif source.stem == "stars":
    replace('rb.PLUGIN_DATA_DIR', 'rb.PLUGIN_DEMOS_DATA_DIR')
    replace('    file:write(start_x', '    if not file then return end\n    file:write(start_x')
    cut("if not rb.SIMULATOR then", "if start_x and start_y then",
        "while not action_quit() do _lcd:update(); rb.sleep(2) end\n\n")
elif source.stem == "menubuttons":
    cut("local oldrb = rb", "-- Menu Button definitions --", 'require("actions")\n\n')
    replace("rb = oldrb\n", "")
elif source.stem == "rb_poly":
    replace("    rb.yield()", "    rb.sleep(2)")
    replace("rb.splash_scroller(10 * rb.HZ, table.concat(s_t))",
            'rb.view_text("Polygon Demo", table.concat(s_t))')
elif source.stem == "time_date":
    replace("local ctx = 0 --CONTEXT_STD",
            'require("actions")\nlocal ctx = rb.contexts.CONTEXT_WPS')
    replace("rb.splash_scroller(10 * rb.HZ, table.concat(s_t))",
            'rb.view_text("Time and Date", table.concat(s_t))')
elif source.stem == "memchk":
    replace("b[1] = rb.new_image(loops, alloc_sz);",
            "b[1] = rb.new_image(rb.LCD_WIDTH, rb.LCD_HEIGHT)")
    replace("rb.lcd_putsxy(0, 20, loops)", "rb.lcd_putsxy(0, 20, tostring(loops))")
    replace("rb.splash_scroller(10 * rb.HZ, table.concat(s_t))",
            'rb.view_text("Memory Check", table.concat(s_t))')
elif source.stem == "rlimg":
    replace('local _print = require("print") -- advanced text printing\n', '')
    replace('require("printmenus") --menu', '')
    script = script.replace('_print.f("%d", _timer.stop(timer))', '_timer.stop(timer)')
    replace("    print_menu(mt, ft)", """    local title = table.remove(mt, 1)
    local selected = 0
    while true do
        selected = rb.do_menu(title, mt, selected)
        if selected < 0 or ft[selected + 2]() then return end
    end""")
    for name in ("pix", "random", "rainbow", "rocklua"):
        script = script.replace('"/' + name + '.bmp"',
                                'rb.PLUGIN_DEMOS_DATA_DIR .. "/lua-' + name + '.bmp"')
    replace('"pix.bmp"', 'rb.PLUGIN_DEMOS_DATA_DIR .. "/lua-pix.bmp"')
elif source.stem == "image_save":
    replace("file = io.open('/' .. name, \"w+\")", 'file = io.open(name, "w")')
    a = script.index('        local cmp = ')
    b = script.index("\n", script.index("0xFF) end}", a)) + 1
    script = script[:a] + """        local cmp = {
            r = function(c) local r = rb.lcd_rgbunpack(c); return r end,
            g = function(c) local _, g = rb.lcd_rgbunpack(c); return g end,
            b = function(c) local _, _, b = rb.lcd_rgbunpack(c); return b end,
        }
""" + script[b:]
    replace('        file:close()', '        file:close()\n        return true')

output.write_text(script)
