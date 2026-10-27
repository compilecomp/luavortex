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
print(type(a))

local adders = {}
for i = 1, 3 do
    adders[i] = function()
        return i * 10
    end
end
print(adders[1](), adders[2](), adders[3]())
