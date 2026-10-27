-- String building: pool materialization, concat, and methods.
local acc = ""
for i = 1, 2000 do
    acc = acc .. ("x" .. i):rep(1)
end
print(#acc)
