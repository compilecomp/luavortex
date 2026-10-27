local point = {x = 3, y = 4}
print(point.x, point.y)
point.z = point.x * point.x + point.y * point.y
print(point.z)

local list = {10, 20, 30, 40}
print(#list)
local sum = 0
for i = 1, #list do
    sum = sum + list[i]
end
print(sum)

for index, value in ipairs(list) do
    print(index, value)
end

local mixed = {1, 2, name = "mix", [10] = "ten"}
print(mixed[1], mixed[2], mixed.name, mixed[10])
mixed[2] = nil
print(#mixed)
