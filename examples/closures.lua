-- Closures: counters share captured cells.
function make_counter()
    local count = 0
    return function()
        count = count + 1
        return count
    end
end

local a = make_counter()
local b = make_counter()
print(a(), a(), a())
print(b(), b())
print(type(a), type(b))
