-- Generic for over pairs with a hash part.
local conf = {a = 1, b = 2, c = 3}
local keys = 0
local total = 0
for k, v in pairs(conf) do
    keys = keys + 1
    total = total + v
end
print(keys, total)
