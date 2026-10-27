-- Tables: constructors, fields, numeric indexing, and length.
local point = {x = 3, y = 4}
print(point.x, point.y)
point.z = point.x * point.x + point.y * point.y
print(point.z, #point)

local list = {10, 20, 30}
local sum = 0
for i = 1, #list do
    sum = sum + list[i]
end
print(sum)

for index, value in ipairs(list) do
    print(index, value)
end
