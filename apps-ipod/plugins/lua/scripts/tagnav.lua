-- Browse native library tags and save a separate tagnavi draft.
-- GPL-2.0-or-later.
local common = require("common")
local fields = {"Artist", "Album artist", "Album", "Genre", "Composer"}
local keys = {"artist", "albumartist", "album", "genre", "composer"}
local chosen = {}
local conjunction = " & "
local page_size = 64
local max_selected = 16
local draft = rb.PLUGIN_APPS_DATA_DIR .. "/luatagnav-draft.config"

local function browse(tag)
    local offset = 0
    while true do
        local rows, more = rb.library_tags(fields[tag], offset)
        if not rows then
            rb.view_text(fields[tag], more)
            return
        end
        local count = #rows
        local selected = chosen[tag] or {}
        chosen[tag] = selected
        local labels = {}
        for i, value in ipairs(rows) do
            labels[i] = selected[value] and "[x] " .. value or value
        end
        if more then labels[#labels + 1] = "Next page" end
        if offset > 0 then labels[#labels + 1] = "Previous page" end
        labels[#labels + 1] = "Back"
        local item = rb.do_menu(fields[tag], labels) + 1
        if item > 0 and item <= count then
            local value = rows[item]
            local total = 0
            for _ in pairs(selected) do total = total + 1 end
            if selected[value] then
                selected[value] = nil
            elseif #value > 48 or value:find('[%c"|]') then
                rb.splash(rb.HZ, "This tag cannot be written to a draft.")
            elseif total == max_selected then
                rb.splash(rb.HZ, "Selection limit reached.")
            else
                selected[value] = true
            end
        elseif more and item == count + 1 then
            offset = offset + count
        elseif offset > 0 and item == count + (more and 2 or 1) then
            offset = math.max(0, offset - page_size)
        else
            return
        end
    end
end

local function save()
    local filters = {}
    for i, key in ipairs(keys) do
        local values = {}
        for value in pairs(chosen[i] or {}) do values[#values + 1] = value end
        table.sort(values)
        if #values > 0 then
            filters[#filters + 1] = key .. ' @ "' .. table.concat(values, "|") .. '"'
        end
    end
    if #filters == 0 then
        rb.splash(rb.HZ, "Select a tag first.")
        return
    end
    local entry = '"Lua filter" -> title = "fmt_title" ? ' .. table.concat(filters, conjunction)
    if #entry > 254 then
        rb.splash(rb.HZ, "Select fewer tags to fit the draft.")
        return
    end
    local data = '#! rockbox/tagbrowser/2.0\n%menu_start "custom" "Database"\n'
        .. entry .. '\n'
    if common.save(draft, data) then rb.view_text("Saved Tagnavi Draft", draft) end
end

local selected = 0
while true do
    local menu = {unpack(fields)}
    menu[#menu + 1] = conjunction == " & " and "Match all fields" or "Match any field"
    menu[#menu + 1] = "Save draft"
    menu[#menu + 1] = "Exit"
    selected = rb.do_menu("Library Tags", menu, selected)
    if selected < 0 or selected == 7 then return end
    if selected < 5 then
        browse(selected + 1)
    elseif selected == 5 then
        conjunction = conjunction == " & " and " | " or " & "
    else
        save()
    end
end
