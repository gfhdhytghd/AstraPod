-- Utilities shared by bundled Lua scripts. GPL-2.0-or-later.
local common = {}

function common.fields(values)
    local lines = {}
    for name, value in pairs(values) do
        lines[#lines + 1] = name .. ": " .. tostring(value)
    end
    table.sort(lines)
    return table.concat(lines, "\n")
end

function common.save(path, text)
    local file = io.open(path, "w")
    if not file then
        rb.splash(rb.HZ, "Could not open " .. path)
        return false
    end
    local written = file:write(text)
    -- The viewer's close commits the sibling file only after sync.
    local closed, result = pcall(file.close, file)
    if not written or not closed or not result then
        rb.splash(rb.HZ, "Could not save " .. path)
        return false
    end
    return true
end

return common
