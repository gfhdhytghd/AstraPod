-- List functions available in this build of Rock Lua. GPL-2.0-or-later.
local libraries = {"_G", "rb", "rockev", "bit", "os", "io", "luadir",
                   "table", "math", "string", "actions", "rbsettings"}
local selected = 0
while true do
    selected = rb.do_menu("Lua Functions", libraries, selected)
    if selected < 0 then return end
    local name = libraries[selected + 1]
    local scope = _G[name]
    if name == "actions" or name == "rbsettings" then
        require(name)
        scope = name == "actions" and rb.actions or rb.settings
    end
    local names = {}
    for key, value in pairs(scope) do
        if type(value) == "function" then names[#names + 1] = key .. "()" end
    end
    table.sort(names)
    rb.view_text(name, #names > 0 and table.concat(names, "\n") or "(constants only)")
end
