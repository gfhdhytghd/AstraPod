-- Directory-by-directory file browser with the stock sort choices.
-- GPL-2.0-or-later.
require("actions")
local common = require("common")
local sorts = {"name", "size", "date", "type"}
local choice = rb.do_menu("File Browser", {
    "Sort by Name", "Sort by Size", "Sort by Date", "Sort by Type", "Exit"
})
local key = sorts[choice + 1]
if not key then return end

local function read_directory(path)
    local entries = {}
    local next_entry, handle = luadir.dir(path, true)
    for name, directory, info in next_entry, handle do
        if name ~= "." and name ~= ".." then
            entries[#entries + 1] = {
                name = name, directory = directory, size = info.size,
                date = info.time, type = name:match("%.([^%.]+)$") or "",
            }
        end
        local action = rb.get_plugin_action(0)
        if action == rb.actions.PLA_EXIT then
            handle:close()
            return
        end
        if #entries == 512 then
            handle:close()
            break
        end
    end
    table.sort(entries, function(a, b)
        if a.directory ~= b.directory then return a.directory end
        if a[key] ~= b[key] then return a[key] < b[key] end
        return a.name < b.name
    end)
    return entries
end

local directories = {"/"}
while #directories > 0 do
    local path = directories[#directories]
    local ok, entries = pcall(read_directory, path)
    if not ok then
        rb.splash(rb.HZ, "Could not read " .. path)
        return
    end
    if not entries then return end
    local labels = {}
    for i, entry in ipairs(entries) do
        labels[i] = entry.name .. (entry.directory and "/" or "")
    end
    local title = #entries == 512 and path .. " (first 512)" or path
    labels[#labels + 1] = "Back"
    local index = rb.do_menu(title, labels)
    local entry = entries[index + 1]
    if not entry then
        table.remove(directories)
    elseif entry.directory then
        directories[#directories + 1] = path .. entry.name .. "/"
    else
        rb.view_text("File details", path .. entry.name .. "\n\n" .. common.fields({
            size = entry.size .. " bytes", modified = entry.date, extension = entry.type
        }))
    end
    entries = nil
    collectgarbage("collect")
end
