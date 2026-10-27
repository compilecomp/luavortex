-- Metatables: __index as a table and as a function.
local defaults = {color = "blue", size = 10}
local shape = setmetatable({}, {__index = defaults})
print(shape.color, shape.size)

local fallback = setmetatable({}, {
    __index = function(t, k)
        return "fallback:" .. k
    end
})
print(fallback.anything)
