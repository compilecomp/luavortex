-- Hash-table conformance: key unification, deletion, identity keys,
-- rehash-heavy growth, and iteration after churn.
local t = {}
t[1] = "int"
print(t[1.0])
t[1.5] = "float"
print(t[1.5])
t[1] = nil
print(t[1])
t[1] = "again"
print(t[1])

local zt = {}
zt[-0.0] = "zero"
print(zt[0], zt[-0.0])

local u = {}
for i = 1, 200 do
    u[i] = i * i
end
for i = 1, 200, 2 do
    u[i] = nil
end
local total = 0
for i = 2, 200, 2 do
    total = total + u[i]
end
print(total)

local w = {}
for i = 1, 100 do
    w["key" .. i] = i
end
for i = 1, 100, 3 do
    w["key" .. i] = nil
end
local wsum = 0
for i = 1, 100 do
    if w["key" .. i] then
        wsum = wsum + w["key" .. i]
    end
end
print(wsum)

local kt = {}
local ka = {}
local kb = {}
kt[ka] = "first"
kt[kb] = "second"
print(kt[ka], kt[kb])
kt[ka] = nil
print(kt[ka], kt[kb])

local fk = {}
local function f1()
    return 1
end
local function f2()
    return 2
end
fk[f1] = 10
fk[f2] = 20
print(fk[f1] + fk[f2])

local bt = {}
bt[true] = "yes"
bt[false] = "no"
print(bt[true], bt[false])

local big = {}
for i = 1, 2500 do
    big["n" .. i] = i
end
local bsum = 0
for i = 1, 2500 do
    bsum = bsum + big["n" .. i]
end
print(bsum)

local cnt = 0
for _ in pairs(w) do
    cnt = cnt + 1
end
print(cnt)
