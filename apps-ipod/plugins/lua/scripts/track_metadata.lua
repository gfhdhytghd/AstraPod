-- Export the native current track snapshot. GPL-2.0-or-later.
local common = require("common")
local track = rb.track()
if not track then
    rb.view_text("Track Metadata", "No track loaded.")
    return
end

local text = common.fields(track)
if common.save(rb.PLUGIN_APPS_DATA_DIR .. "/lua-metadata.txt", text .. "\n") then
    rb.view_text("Track Metadata", text)
end
if rb.do_menu("Track Metadata", {"Save cached album artwork", "Exit"}, 1) == 0 then
    local art = rb.track_art()
    if not art then
        rb.splash(rb.HZ, "No cached album artwork.")
    elseif require("image_save")(art, rb.PLUGIN_APPS_DATA_DIR .. "/lua-albumart.bmp") then
        rb.splash(rb.HZ, "Saved lua-albumart.bmp")
    end
end
