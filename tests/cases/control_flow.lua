local total = 0
for i = 1, 10 do
    if i % 2 == 0 then
        total = total + i
    end
end
print(total)

local n = 0
repeat
    n = n + 1
until n >= 3
print(n)

local x = 5
while x > 0 do
    x = x - 2
end
print(x)

for i = 10, 1, -3 do
    print(i)
end

for i = 1, 5 do
    if i == 4 then
        break
    end
    print(i)
end
