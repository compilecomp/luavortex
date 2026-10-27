print(tostring(42), tostring(true), tostring(nil))
print(tonumber("37"), tonumber("0x10", 16), tonumber("nope"))
print(type(1), type("s"), type(nil), type({}))
print(2 ^ 0.5 == math.sqrt(2))
local v = 7
print(v // 2, v % 2, -v)
print(5 < 10, 5 > 10, 5 <= 5, 7 >= 9, 1 == 1.0, "a" ~= "b")
print(3 & 5, 3 | 5, 3 ~ 5, 1 << 4, 256 >> 4)
print(#"abcd")
