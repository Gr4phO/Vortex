const std = @import("std");

const Node = struct {
    x: i64,
    y: i64,
    z: i64,
};

fn fib(n: i64) i64 {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

inline fn stepFn(a: i64, b: i64) i64 {
    var next = a + b;
    if (next >= 1000003) next -= 1000003;
    return next;
}

fn rowWorker(start: usize, end: usize, cols: usize, row_sums: []i64) void {
    var r = start;
    while (r < end) : (r += 1) {
        var acc: i64 = @intCast(r);
        var c: usize = 0;
        while (c < cols) : (c += 1) {
            acc += 7919;
            if (acc >= 1000003) acc -= 1000003;
        }
        row_sums[r] = acc;
    }
}

pub fn main() !void {
    const alloc = std.heap.page_allocator;
    const t_all = std.time.microTimestamp();

    // Task 1
    const reps1: usize = 100;
    const n1: usize = 1_000_000;
    const step1: f32 = 0.000001;
    var total_pi1: f32 = 0.0;
    const t1 = std.time.microTimestamp();
    for (0..reps1) |_| {
        var sum1: f32 = 0.0;
        var x1: f32 = 0.0000005;
        for (0..n1) |_| {
            sum1 = sum1 + 4.0 / (1.0 + x1 * x1);
            x1 = x1 + step1;
        }
        total_pi1 += sum1 * step1;
    }
    const us1 = std.time.microTimestamp() - t1;
    const pi1 = total_pi1 * 0.01;

    // Task 2
    const n2: usize = 50_000_000;
    const t2 = std.time.microTimestamp();
    const arr2 = try alloc.alloc(i64, n2);
    var cur2: i64 = 1;
    for (arr2) |*slot| {
        cur2 += 7;
        if (cur2 >= 1024) cur2 -= 1024;
        slot.* = cur2;
    }
    var sum2: i64 = 0;
    for (arr2) |*slot| {
        const v = slot.* * 3 + 1;
        slot.* = v;
        sum2 += v;
    }
    alloc.free(arr2);
    const us2 = std.time.microTimestamp() - t2;

    // Task 3
    const n3: usize = 1_000_000;
    const t3 = std.time.microTimestamp();
    var arena = std.heap.ArenaAllocator.init(alloc);
    const aalloc = arena.allocator();
    const nodes3 = try aalloc.alloc(*Node, n3);
    for (0..n3) |i| {
        const nd = try aalloc.create(Node);
        const iv: i64 = @intCast(i);
        nd.* = .{ .x = iv, .y = iv * 2, .z = iv + 5 };
        nodes3[i] = nd;
    }
    var sum3: i64 = 0;
    for (nodes3) |p| sum3 += p.x + p.y + p.z;
    arena.deinit();
    const us3 = std.time.microTimestamp() - t3;

    // Task 4
    const t4 = std.time.microTimestamp();
    const f40 = fib(40);
    var acc4: i64 = 0;
    for (0..100_000_000) |_| acc4 = stepFn(acc4, 7919);
    const us4 = std.time.microTimestamp() - t4;
    const chk4 = f40 + acc4;

    // Task 5
    const rows5: usize = 2000;
    const cols5: usize = 50000;
    const t5 = std.time.microTimestamp();
    const row_sums = try alloc.alloc(i64, rows5);
    defer alloc.free(row_sums);
    const cpus = try std.Thread.getCpuCount();
    const workers = @min(cpus, 64);
    var threads: [64]std.Thread = undefined;
    const chunk = rows5 / workers;
    const rem = rows5 % workers;
    var cur: usize = 0;
    for (0..workers) |w| {
        const sz = chunk + (if (w < rem) @as(usize, 1) else 0);
        threads[w] = try std.Thread.spawn(.{}, rowWorker, .{ cur, cur + sz, cols5, row_sums });
        cur += sz;
    }
    for (0..workers) |w| threads[w].join();
    var sum5: i64 = 0;
    for (row_sums) |rs| sum5 += rs;
    const us5 = std.time.microTimestamp() - t5;

    const us_total = std.time.microTimestamp() - t_all;
    const out = std.io.getStdOut().writer();
    try out.print("TASK1_US {d}\nTASK1_PI {d:.6}\nTASK2_US {d}\nTASK2_CHK {d}\n", .{ us1, pi1, us2, sum2 });
    try out.print("TASK3_US {d}\nTASK3_CHK {d}\nTASK4_US {d}\nTASK4_CHK {d}\n", .{ us3, sum3, us4, chk4 });
    try out.print("TASK5_US {d}\nTASK5_CHK {d}\nTOTAL_US {d}\n", .{ us5, sum5, us_total });
}
