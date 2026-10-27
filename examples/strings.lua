-- Strings: literals, concat, methods, and numeric coercion.
local name = "vortex"
print("hello, " .. name .. "!")
print(name:upper())
print(("abc"):rep(3))
print(#name, name:sub(1, 3))
print("10" + 5)
print(2 ^ 10, 7 // 2, 7 % 3, -(3 + 1))
