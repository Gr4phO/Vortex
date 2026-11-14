-- LuaJIT 2.1 Benchmark Suite (5 Identical Algorithmic Tasks)
local ffi = require("ffi")

ffi.cdef[[
typedef struct { int64_t x, y, z; } Node;
]]

local function now_us()
    return math.floor(os.clock() * 1000000)
end

local function fib(n)
    if n <= 1 then return n end
    return fib(n - 1) + fib(n - 2)
end

local function step_fn(a, b)
    local next_v = a + b
    if next_v >= 1000003 then
        next_v = next_v - 1000003
    end
    return next_v
end

local t_all = now_us()

-- Task 1 (f32 semantics via float[1])
local reps1 = 100
local n1 = 1000000
local step1 = ffi.cast("float", 0.000001)
local total_pi1 = ffi.cast("float", 0.0)
local four = ffi.cast("float", 4.0)
local one = ffi.cast("float", 1.0)
local t1_0 = now_us()
for r = 0, reps1 - 1 do
    local sum1 = ffi.cast("float", 0.0)
    local x1 = ffi.cast("float", 0.0000005)
    for i = 0, n1 - 1 do
        sum1 = ffi.cast("float", sum1 + ffi.cast("float", four / ffi.cast("float", one + ffi.cast("float", x1 * x1))))
        x1 = ffi.cast("float", x1 + step1)
    end
    total_pi1 = ffi.cast("float", total_pi1 + ffi.cast("float", sum1 * step1))
end
local us1 = now_us() - t1_0
local pi1 = tonumber(total_pi1 * 0.01)

-- Task 2 (50,000,000 int64 elements)
local n2 = 50000000
local t2_0 = now_us()
local arr2 = ffi.new("int64_t[?]", n2)
local cur2 = 1
for i = 0, n2 - 1 do
    cur2 = cur2 + 7
    if cur2 >= 1024 then cur2 = cur2 - 1024 end
    arr2[i] = cur2
end
local sum2 = 0LL
for i = 0, n2 - 1 do
    local v = arr2[i] * 3 + 1
    arr2[i] = v
    sum2 = sum2 + v
end
arr2 = nil
local us2 = now_us() - t2_0

-- Task 3 (1,000,000 Node structs + reduction)
local n3 = 1000000
local t3_0 = now_us()
local nodes3 = ffi.new("Node[?]", n3)
for i = 0, n3 - 1 do
    nodes3[i].x = i
    nodes3[i].y = i * 2
    nodes3[i].z = i + 5
end
local sum3 = 0LL
for i = 0, n3 - 1 do
    local p = nodes3[i]
    sum3 = sum3 + p.x + p.y + p.z
end
nodes3 = nil
local us3 = now_us() - t3_0

-- Task 4 (fib(40) + 100M step_fn calls)
local t4_0 = now_us()
local f40 = fib(40)
local acc4 = 0
for i = 0, 99999999 do
    acc4 = step_fn(acc4, 7919)
end
local us4 = now_us() - t4_0
local chk4 = f40 + acc4

-- Task 5 (single-VM reference: LuaJIT has no built-in shared-memory multithreading)
local rows5 = 2000
local cols5 = 50000
local t5_0 = now_us()
local row_sums = ffi.new("int64_t[?]", rows5)
for r = 0, rows5 - 1 do
    local acc = r
    for c = 0, cols5 - 1 do
        acc = acc + 7919
        if acc >= 1000003 then acc = acc - 1000003 end
    end
    row_sums[r] = acc
end
local sum5 = 0LL
for r = 0, rows5 - 1 do
    sum5 = sum5 + row_sums[r]
end
local us5 = now_us() - t5_0

local us_total = now_us() - t_all
print(string.format("TASK1_US %d", us1))
print(string.format("TASK1_PI %.6f", pi1))
print(string.format("TASK2_US %d", us2))
print("TASK2_CHK " .. tostring(sum2):gsub("LL$", ""))
print(string.format("TASK3_US %d", us3))
print("TASK3_CHK " .. tostring(sum3):gsub("LL$", ""))
print(string.format("TASK4_US %d", us4))
print(string.format("TASK4_CHK %d", chk4))
print(string.format("TASK5_US %d", us5))
print("TASK5_CHK " .. tostring(sum5):gsub("LL$", ""))
print(string.format("TOTAL_US %d", us_total))
