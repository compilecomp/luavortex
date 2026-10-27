local x <const> = 10
print(x)

do
    local y = 1
    do
        local z = 2
        y = y + z
    end
    print(y)
end

local a, b = 1, 2
a, b = b, a
print(a, b)

local c, d = 1
print(c, d)
