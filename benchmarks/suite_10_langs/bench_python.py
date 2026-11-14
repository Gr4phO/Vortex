import multiprocessing as mp
import os
import sys
import time


class Node:
    __slots__ = ("x", "y", "z")

    def __init__(self, x: int, y: int, z: int) -> None:
        self.x = x
        self.y = y
        self.z = z


def fib(n: int) -> int:
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)


def step_fn(a: int, b: int) -> int:
    nxt = a + b
    if nxt >= 1000003:
        nxt -= 1000003
    return nxt


def _worker_rows(args):
    start, end, cols = args
    out = []
    for r in range(start, end):
        acc = r
        for _ in range(cols):
            acc += 7919
            if acc >= 1000003:
                acc -= 1000003
        out.append(acc)
    return out


def main() -> None:
    t_all = time.perf_counter_ns()

    # Task 1 (100,000,000 iterations)
    n1 = 100_000_000
    step1 = 0.00000001
    sum1 = 0.0
    x1 = 0.000000005
    t1_0 = time.perf_counter_ns()
    for _ in range(n1):
        sum1 = sum1 + 4.0 / (1.0 + x1 * x1)
        x1 = x1 + step1
    us1 = (time.perf_counter_ns() - t1_0) // 1000
    pi1 = sum1 * step1

    # Task 2 (50,000,000 elements)
    n2 = 50_000_000
    t2_0 = time.perf_counter_ns()
    arr2 = [0] * n2
    cur2 = 1
    for i in range(n2):
        cur2 += 7
        if cur2 >= 1024:
            cur2 -= 1024
        arr2[i] = cur2
    sum2 = 0
    for i in range(n2):
        v = arr2[i] * 3 + 1
        arr2[i] = v
        sum2 += v
    del arr2
    us2 = (time.perf_counter_ns() - t2_0) // 1000

    # Task 3 (1,000,000 Node objects)
    n3 = 1_000_000
    t3_0 = time.perf_counter_ns()
    nodes3 = [Node(i, i * 2, i + 5) for i in range(n3)]
    sum3 = 0
    for p in nodes3:
        sum3 += p.x + p.y + p.z
    del nodes3
    us3 = (time.perf_counter_ns() - t3_0) // 1000

    # Task 4 (fib(40) + 100M step_fn calls)
    t4_0 = time.perf_counter_ns()
    f40 = fib(40)
    acc4 = 0
    for _ in range(100_000_000):
        acc4 = step_fn(acc4, 7919)
    us4 = (time.perf_counter_ns() - t4_0) // 1000
    chk4 = f40 + acc4

    # Task 5 (2000 x 50000 = 100M cells across logical CPUs via multiprocessing)
    rows5 = 2000
    cols5 = 50000
    t5_0 = time.perf_counter_ns()
    workers = os.cpu_count() or 1
    chunk = rows5 // workers
    rem = rows5 % workers
    spans = []
    cur = 0
    for w in range(workers):
        sz = chunk + (1 if w < rem else 0)
        spans.append((cur, cur + sz, cols5))
        cur += sz
    with mp.Pool(processes=workers) as pool:
        parts = pool.map(_worker_rows, spans)
    sum5 = sum(v for sub in parts for v in sub)
    us5 = (time.perf_counter_ns() - t5_0) // 1000

    us_total = (time.perf_counter_ns() - t_all) // 1000

    print(f"TASK1_US {us1}")
    print(f"TASK1_PI {pi1:.6f}")
    print(f"TASK2_US {us2}")
    print(f"TASK2_CHK {sum2}")
    print(f"TASK3_US {us3}")
    print(f"TASK3_CHK {sum3}")
    print(f"TASK4_US {us4}")
    print(f"TASK4_CHK {chk4}")
    print(f"TASK5_US {us5}")
    print(f"TASK5_CHK {sum5}")
    print(f"TOTAL_US {us_total}")


if __name__ == "__main__":
    main()
