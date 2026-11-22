-- Fade the native volume while the sleep timer runs. GPL-2.0-or-later.
require("actions")
require("rbsettings")
local choice = rb.do_menu("Fade to Sleep", {"1 minute", "5 minutes", "10 minutes",
                         "15 minutes", "30 minutes", "Exit"})
local minutes = ({1, 5, 10, 15, 30})[choice + 1]
if not minutes then return end
if bit.band(rb.audio("status"), rb.AUDIO_STATUS_PLAY) == 0 then
    rb.splash(rb.HZ, "No track playing.")
    return
end
if not rb.set_sleep_timer(minutes) then return end

local duration = minutes * 60 * rb.HZ
local start = rb.current_tick()
local original = rb.settings.read("global_status", "volume")
local volume = original
local minimum = rb.minimum_volume()
while true do
    local elapsed = rb.current_tick() - start
    if elapsed >= duration then break end
    if bit.band(rb.audio("status"), rb.AUDIO_STATUS_PLAY) == 0 then return end
    local value = original - (original - minimum) * elapsed / duration
    if value < volume then
        if not rb.fade_volume(volume, value) then
            rb.splash(rb.HZ, "Volume changed. Sleep timer remains active.")
            return
        end
        volume = value
    end
    rb.lcd_clear_display()
    rb.lcd_puts(0, 0, "Fade to Sleep")
    rb.lcd_puts(0, 1, string.format("%d seconds remaining", (duration - elapsed) / rb.HZ))
    rb.lcd_puts(0, 2, "Menu: leave fade")
    rb.lcd_update()
    local action = rb.get_action(rb.contexts.CONTEXT_WPS, rb.HZ / 4)
    if action == rb.actions.ACTION_WPS_MENU or action == rb.actions.ACTION_STD_CANCEL then
        return
    end
end
