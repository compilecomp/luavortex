-- Tight arithmetic loop: integer adds and compares through the typed paths.
local sum = 0
for i = 1, 200000 do
    sum = sum + i % 7 - (i // 3)
end
print(sum)
