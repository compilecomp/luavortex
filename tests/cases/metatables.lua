local defaults = {color = "blue", size = 10}
local shape = setmetatable({}, {__index = defaults})
print(shape.color, shape.size)

local fallback = setmetatable({}, {
    __index = function(t, k)
        return "fallback:" .. k
    end
})
print(fallback.anything)
print(fallback.other)

local direct = setmetatable({own = 1}, {__index = fallback})
print(direct.own, direct.missing)

local raw_store = {}
local sink = setmetatable({}, {__newindex = function(t, k, v)
    raw_store[k] = v
end})
sink.x = 42
print(sink.x, raw_store.x)
