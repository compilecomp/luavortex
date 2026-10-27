function add(a, b)
    return a + b
end

local function compose_result(x)
    local doubled = add(x, x)
    return doubled
end

print(add(2, 3))
print(compose_result(10))

function outer()
    local hidden = 7
    local function peek()
        return hidden
    end
    hidden = 8
    return peek()
end
print(outer())

function variadic_ignored(a, b)
    return a + b
end
print(variadic_ignored(1, 2, 99))
