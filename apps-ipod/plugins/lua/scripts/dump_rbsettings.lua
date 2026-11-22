-- Native read-only settings snapshot. GPL-2.0-or-later.
require("rbsettings")
local common = require("common")
local text = "global_settings\n" .. common.fields(rb.settings.dump("global_settings"))
          .. "\n\nglobal_status\n" .. common.fields(rb.settings.dump("global_status"))
local path = rb.PLUGIN_APPS_DATA_DIR .. "/lua-settings.txt"
if common.save(path, text .. "\n") then
    rb.view_text("Settings snapshot", text)
end
