import math
import time


def qsort_i64(arr, lo, hi):
    i, j = lo, hi
    pivot = arr[(lo + hi) // 2]
    while i <= j:
        while arr[i] < pivot:
            i += 1
        while arr[j] > pivot:
            j -= 1
        if i <= j:
            arr[i], arr[j] = arr[j], arr[i]
            i += 1
            j -= 1
    if lo < j:
        qsort_i64(arr, lo, j)
    if i < hi:
        qsort_i64(arr, i, hi)


def main():
    n1, steps1 = 600, 25
    n2 = 500000
    n3 = 320
    n4 = 10000000
    w5, iter5 = 1024, 256

    t_all0 = time.perf_counter()

    # T1
    x = [((i * 17) % 100) * 0.1 - 5.0 for i in range(n1)]
    y = [((i * 31) % 100) * 0.1 - 5.0 for i in range(n1)]
    z = [((i * 47) % 100) * 0.1 - 5.0 for i in range(n1)]
    vx = [((i % 7) - 3) * 0.01 for i in range(n1)]
    vy = [(((i * 3) % 7) - 3) * 0.01 for i in range(n1)]
    vz = [(((i * 5) % 7) - 3) * 0.01 for i in range(n1)]
    m = [0.5 + (i % 5) * 0.25 for i in range(n1)]
    dt, soft = 0.01, 0.01
    t1_0 = time.perf_counter()
    sqrt = math.sqrt
    for _ in range(steps1):
        for i in range(n1):
            fx = fy = fz = 0.0
            xi, yi, ziv = x[i], y[i], z[i]
            for j in range(n1):
                if j != i:
                    dx = x[j] - xi
                    dy = y[j] - yi
                    dz = z[j] - ziv
                    d2 = dx * dx + dy * dy + dz * dz + soft
                    inv = 1.0 / sqrt(d2)
                    f = m[j] * inv * inv * inv
                    fx += dx * f
                    fy += dy * f
                    fz += dz * f
            vx[i] += fx * dt
            vy[i] += fy * dt
            vz[i] += fz * dt
        for i in range(n1):
            x[i] += vx[i] * dt
            y[i] += vy[i] * dt
            z[i] += vz[i] * dt
    chk1 = sum(0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]) for i in range(n1))
    us1 = int((time.perf_counter() - t1_0) * 1_000_000)

    # T2
    arr2 = [0] * n2
    lcg = 12345
    for i in range(n2):
        lcg = (lcg * 48271) % 2147483647
        arr2[i] = lcg
    t2_0 = time.perf_counter()
    qsort_i64(arr2, 0, n2 - 1)
    chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1]
    us2 = int((time.perf_counter() - t2_0) * 1_000_000)

    # T3
    a3 = [((i * 7 + k * 13) % 97) * 0.01 for i in range(n3) for k in range(n3)]
    b3 = [((i * 11 + k * 5) % 89) * 0.01 for i in range(n3) for k in range(n3)]
    c3 = [0.0] * (n3 * n3)
    t3_0 = time.perf_counter()
    for i in range(n3):
        coff = i * n3
        for k in range(n3):
            aik = a3[coff + k]
            boff = k * n3
            for j in range(n3):
                c3[coff + j] += aik * b3[boff + j]
    chk3 = sum(c3)
    us3 = int((time.perf_counter() - t3_0) * 1_000_000)

    # T4
    is_comp = bytearray(n4)
    t4_0 = time.perf_counter()
    p = 2
    while p * p < n4:
        if not is_comp[p]:
            for m4 in range(p * p, n4, p):
                is_comp[m4] = 1
        p += 1
    chk4 = sum(i for i in range(2, n4) if not is_comp[i])
    us4 = int((time.perf_counter() - t4_0) * 1_000_000)

    # T5 (single-thread pure Python due to GIL)
    t5_0 = time.perf_counter()
    chk5 = 0
    for py in range(w5):
        cy = -1.25 + (py * 2.5) / w5
        rsum = 0
        for px in range(w5):
            cx = -2.0 + (px * 3.0) / w5
            zr = zi = 0.0
            it = 0
            while it < iter5 and (zr * zr + zi * zi <= 4.0):
                tmp = zr * zr - zi * zi + cx
                zi = 2.0 * zr * zi + cy
                zr = tmp
                it += 1
            rsum += it
        chk5 += rsum
    us5 = int((time.perf_counter() - t5_0) * 1_000_000)

    total_us = int((time.perf_counter() - t_all0) * 1_000_000)
    print(f"T1_US {us1}\nT1_CHK {chk1:.6f}")
    print(f"T2_US {us2}\nT2_CHK {chk2}")
    print(f"T3_US {us3}\nT3_CHK {chk3:.6f}")
    print(f"T4_US {us4}\nT4_CHK {chk4}")
    print(f"T5_US {us5}\nT5_CHK {chk5}")
    print(f"TOTAL_US {total_us}")


if __name__ == "__main__":
    main()
