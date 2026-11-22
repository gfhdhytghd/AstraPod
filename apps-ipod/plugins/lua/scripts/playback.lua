-- Control current native playback. GPL-2.0-or-later.
require("actions")
local actions = rb.actions
local quit, refresh = false, true
local function status_has(flag)
    return bit.band(rb.audio("status"), flag) ~= 0
end
rockev.register("action", function(action)
    if action == actions.PLA_EXIT or action == actions.PLA_CANCEL then
        quit = true
    elseif action == actions.PLA_SELECT then
        if status_has(rb.AUDIO_STATUS_PAUSE) then
            rb.audio("resume")
        elseif status_has(rb.AUDIO_STATUS_PLAY) then
            rb.audio("pause")
        end
    elseif action == actions.PLA_LEFT then
        rb.audio("prev")
    elseif action == actions.PLA_RIGHT then
        rb.audio("next")
    elseif action == actions.PLA_LEFT_REPEAT then
        rb.audio("seek", (rb.audio("elapsed") or 0) - 5000)
    elseif action == actions.PLA_RIGHT_REPEAT then
        rb.audio("seek", (rb.audio("elapsed") or 0) + 5000)
    end
    refresh = true
end)
rockev.register("playback", function() refresh = true end)
rockev.register("timer", function() refresh = true end, rb.HZ / 4)
while not quit do
    if refresh then
        local track = rb.track()
        rb.lcd_clear_display()
        rb.lcd_puts(0, 0, "Playback")
        if track then
            local title = track.title or track.path
            rb.lcd_puts(0, 1, title)
            rb.lcd_puts(0, 2, string.format("%s  %d / %d seconds",
                status_has(rb.AUDIO_STATUS_PAUSE) and "Paused" or "Playing",
                track.elapsed / 1000, track.length / 1000))
            rb.lcd_puts(0, 4, "Select: pause or resume")
            rb.lcd_puts(0, 5, "Left/Right: skip or hold to seek")
        else
            rb.lcd_puts(0, 1, "No track loaded.")
        end
        rb.lcd_puts(0, 6, "Menu: exit")
        rb.lcd_update()
        refresh = false
    end
    rb.sleep(2)
end
