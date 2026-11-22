-- Sample tracks from the native database and save a playlist. GPL-2.0-or-later.
local common = require("common")
local selected = rb.do_menu("Random Playlist", {"25 tracks", "100 tracks", "500 tracks", "Exit"})
local count = ({25, 100, 500})[selected + 1]
if not count then return end
rb.splash(0, "Reading library. Menu: cancel")
local tracks, reason = rb.random_tracks(count)
if not tracks then
    rb.view_text("Random Playlist", reason)
    return
end
local paths = {}
for _, path in ipairs(tracks) do
    if path:sub(1, 1) == "/" and not path:find("[\r\n]") and rb.file_exists(path) then
        paths[#paths + 1] = path
    end
end
if #paths == 0 then
    rb.view_text("Random Playlist", "No tracks found in the library.")
    return
end
math.randomseed(rb.current_tick())
for i = #paths, 2, -1 do
    local j = math.random(i)
    paths[i], paths[j] = paths[j], paths[i]
end
if common.save(rb.RANDOM_PLAYLIST, table.concat(paths, "\n") .. "\n") then
    local action = rb.do_menu("Playlist saved", {"Play now", "Exit"}, 1)
    if action == 0 then
        if rb.play_random_playlist() then
            os.exit(rb.PLUGIN_GOTO_WPS)
        end
        rb.splash(rb.HZ, "Could not start the playlist.")
    end
end
