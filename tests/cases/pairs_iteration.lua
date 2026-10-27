local conf = {a = 1, b = 2, c = 3}
local keys = 0
local total = 0
for k, v in pairs(conf) do
    keys = keys + 1
    total = total + v
end
print(keys, total)

local t = {}
t.k1 = "v1"
t.k2 = "v2"
local n = 0
for k, v in pairs(t) do
    n = n + 1
end
print(n)
