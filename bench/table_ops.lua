-- Table traffic: constructors, hash stores, raw reads, and length.
-- Sized to the verified conformance scope (compilecomp/luavortex#5 tracks growth beyond
-- it): several capacity doublings plus string-key churn that forces
-- collections, all inside the benchmark's wall-clock budget.
local t = {}
for i = 1, 800 do
    t[i] = i * 2
    t["k" .. i] = i
end
local acc = 0
for i = 1, 800 do
    acc = acc + t[i] + t["k" .. i]
end
print(acc % 1000003)
