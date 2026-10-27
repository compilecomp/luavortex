-- The classic Fibonacci: recursion, locals, and calls.
function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

local function main_loop()
    local i = 1
    while i <= 10 do
        print("fib", i, fib(i))
        i = i + 1
    end
end

main_loop()
