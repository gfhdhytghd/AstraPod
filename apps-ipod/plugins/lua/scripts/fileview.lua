-- Text and hex previews using the native file picker. GPL-2.0-or-later.
local mode = 0
while true do
    mode = rb.do_menu("File View", {"Text view", "Hex view", "Exit"}, mode)
    if mode < 0 or mode == 2 then return end
    local path = rb.pick_file("Choose File")
    if path then
        local offset = 0
        local bytes = mode == 0 and 768 or 192
        while true do
            local data, size = rb.file_preview(path, offset, bytes)
            if not data then
                rb.splash(rb.HZ, "Could not read the file.")
                break
            end
            local text
            if mode == 0 then
                text = data:gsub("[%z\1-\8\11\12\14-\31]", ".")
            else
                local rows = {}
                for first = 1, #data, 8 do
                    local row = {}
                    for i = first, math.min(#data, first + 7) do
                        row[#row + 1] = string.format("%02x", data:byte(i))
                    end
                    rows[#rows + 1] = string.format("%08x: %s", offset + first - 1,
                                                   table.concat(row, " "))
                end
                text = table.concat(rows, "\n")
            end
            rb.view_text(path:match("[^/]+$") or path, text ~= "" and text or "(empty)")
            local options, positions = {}, {}
            if offset + #data < size and #data > 0 then
                options[#options + 1] = "Next page"
                positions[#positions + 1] = offset + #data
            end
            if offset > 0 then
                options[#options + 1] = "Previous page"
                positions[#positions + 1] = math.max(0, offset - bytes)
            end
            if #options == 0 then break end
            options[#options + 1] = "Close file"
            local page = rb.do_menu("File View", options)
            if not positions[page + 1] then break end
            offset = positions[page + 1]
        end
    end
end
