-- Demonstrate nested menus with independent selection state.
-- GPL-2.0-or-later.
local submenu = {
    {title = "Playback status", items = {"Audio status", "Current track"}},
    {title = "Lua memory", items = {"Pool usage", "Lua usage"}},
}
local choice = 0
while true do
    choice = rb.do_menu("Submenu Demo", {"Playback status", "Lua memory", "Exit"}, choice)
    local menu = submenu[choice + 1]
    if not menu then return end
    local selected = 0
    while true do
        selected = rb.do_menu(menu.title, menu.items, selected)
        if selected < 0 then break end
        local value
        if choice == 0 then
            local track = rb.track()
            value = selected == 0 and rb.audio("status")
                or track and (track.title or track.path) or "No track loaded"
        elseif selected == 0 then
            local high, used, free = rb.mem_stats()
            value = string.format("High: %d\nUsed: %d\nFree: %d bytes", high, used, free)
        else
            value = string.format("%d KiB", collectgarbage("count"))
        end
        rb.view_text(menu.items[selected + 1], tostring(value))
    end
end
