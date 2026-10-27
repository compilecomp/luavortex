local i = 0
::top::
i = i + 1
if i < 3 then goto top end
print(i)

do
    goto done
end
::done::
print("reached")

local sum = 0
for k = 1, 5 do
    if k % 2 == 0 then goto continue end
    sum = sum + k
    ::continue::
end
print(sum)
