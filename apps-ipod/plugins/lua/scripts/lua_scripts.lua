-- Select a bundled script without changing the Lua module search path.
-- GPL-2.0-or-later.
local selected = rb.do_menu("Lua Scripts", rb.scripts())
if selected >= 0 then rb.launch_script(selected + 1) end
