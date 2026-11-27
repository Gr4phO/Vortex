#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>

static int64_t time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void qsort_i64(int64_t* arr, int64_t lo, int64_t hi) {
    int64_t i = lo, j = hi;
    int64_t pivot = arr[(lo + hi) / 2];
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) {
            std::swap(arr[i], arr[j]);
            i++;
            j--;
        }
    }
    if (lo < j) qsort_i64(arr, lo, j);
    if (i < hi) qsort_i64(arr, i, hi);
}

int main() {
    volatile int64_t v_n1 = 600, v_steps1 = 25;
    volatile int64_t v_n2 = 500000;
    volatile int64_t v_n3 = 320;
    volatile int64_t v_n4 = 10000000;
    volatile int64_t v_w5 = 1024, v_iter5 = 256;

    int64_t t_all0 = time_us();

    /* T1 */
    int64_t n1 = v_n1, steps1 = v_steps1;
    std::vector<double> x(n1), y(n1), z(n1), vx(n1), vy(n1), vz(n1), m(n1);
    for (int64_t i = 0; i < n1; i++) {
        x[i] = double((i * 17) % 100) * 0.1 - 5.0;
        y[i] = double((i * 31) % 100) * 0.1 - 5.0;
        z[i] = double((i * 47) % 100) * 0.1 - 5.0;
        vx[i] = double((i % 7) - 3) * 0.01;
        vy[i] = double(((i * 3) % 7) - 3) * 0.01;
        vz[i] = double(((i * 5) % 7) - 3) * 0.01;
        m[i] = 0.5 + double(i % 5) * 0.25;
    }
    const double dt = 0.01, soft = 0.01;
    int64_t t1_0 = time_us();
    for (int64_t s = 0; s < steps1; s++) {
        for (int64_t i = 0; i < n1; i++) {
            double fx = 0.0, fy = 0.0, fz = 0.0;
            double xi = x[i], yi = y[i], zi_v = z[i];
            for (int64_t j = 0; j < n1; j++) {
                if (j != i) {
                    double dx = x[j] - xi;
                    double dy = y[j] - yi;
                    double dz = z[j] - zi_v;
                    double d2 = dx * dx + dy * dy + dz * dz + soft;
                    double inv = 1.0 / std::sqrt(d2);
                    double f = m[j] * inv * inv * inv;
                    fx += dx * f;
                    fy += dy * f;
                    fz += dz * f;
                }
            }
            vx[i] += fx * dt;
            vy[i] += fy * dt;
            vz[i] += fz * dt;
        }
        for (int64_t i = 0; i < n1; i++) {
            x[i] += vx[i] * dt;
            y[i] += vy[i] * dt;
            z[i] += vz[i] * dt;
        }
    }
    double chk1 = 0.0;
    for (int64_t i = 0; i < n1; i++) {
        chk1 += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    }
    int64_t t1_1 = time_us();

    /* T2 */
    int64_t n2 = v_n2;
    std::vector<int64_t> arr2(n2);
    int64_t lcg = 12345;
    for (int64_t i = 0; i < n2; i++) {
        lcg = (lcg * 48271LL) % 2147483647LL;
        arr2[i] = lcg;
    }
    int64_t t2_0 = time_us();
    qsort_i64(arr2.data(), 0, n2 - 1);
    int64_t chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1];
    int64_t t2_1 = time_us();

    /* T3 */
    int64_t n3 = v_n3;
    std::vector<double> a3(n3 * n3), b3(n3 * n3), c3(n3 * n3, 0.0);
    for (int64_t i = 0; i < n3; i++) {
        for (int64_t k = 0; k < n3; k++) {
            a3[i * n3 + k] = double((i * 7 + k * 13) % 97) * 0.01;
            b3[i * n3 + k] = double((i * 11 + k * 5) % 89) * 0.01;
        }
    }
    int64_t t3_0 = time_us();
    for (int64_t i = 0; i < n3; i++) {
        double* crow = &c3[i * n3];
        for (int64_t k = 0; k < n3; k++) {
            double aik = a3[i * n3 + k];
            const double* brow = &b3[k * n3];
            for (int64_t j = 0; j < n3; j++) {
                crow[j] += aik * brow[j];
            }
        }
    }
    double chk3 = 0.0;
    for (int64_t i = 0; i < n3 * n3; i++) chk3 += c3[i];
    int64_t t3_1 = time_us();

    /* T4 */
    int64_t n4 = v_n4;
    std::vector<int32_t> is_comp(n4, 0);
    int64_t t4_0 = time_us();
    for (int64_t p = 2; p * p < n4; p++) {
        if (is_comp[p] == 0) {
            for (int64_t m4 = p * p; m4 < n4; m4 += p) {
                is_comp[m4] = 1;
            }
        }
    }
    int64_t chk4 = 0;
    for (int64_t i = 2; i < n4; i++) {
        if (is_comp[i] == 0) chk4 += i;
    }
    int64_t t4_1 = time_us();

    /* T5 */
    int64_t w5 = v_w5, iter5 = v_iter5;
    std::vector<int64_t> row_sums(w5, 0);
    int workers = (int)std::thread::hardware_concurrency();
    if (workers <= 0) workers = 1;
    std::vector<std::thread> threads;
    int64_t t5_0 = time_us();
    int64_t chunk = w5 / workers, rem = w5 % workers, cur = 0;
    for (int w = 0; w < workers; w++) {
        int64_t sz = chunk + (w < rem ? 1 : 0);
        int64_t y0 = cur, y1 = cur + sz;
        cur += sz;
        threads.emplace_back([&, y0, y1]() {
            for (int64_t py = y0; py < y1; py++) {
                double cy = -1.25 + (double(py) * 2.5) / double(w5);
                int64_t rsum = 0;
                for (int64_t px = 0; px < w5; px++) {
                    double cx = -2.0 + (double(px) * 3.0) / double(w5);
                    double zr = 0.0, zi = 0.0;
                    int64_t it = 0;
                    while (it < iter5 && (zr * zr + zi * zi <= 4.0)) {
                        double tmp = zr * zr - zi * zi + cx;
                        zi = 2.0 * zr * zi + cy;
                        zr = tmp;
                        it++;
                    }
                    rsum += it;
                }
                row_sums[py] = rsum;
            }
        });
    }
    for (auto& th : threads) th.join();
    int64_t chk5 = 0;
    for (int64_t i = 0; i < w5; i++) chk5 += row_sums[i];
    int64_t t5_1 = time_us();

    int64_t total_us = time_us() - t_all0;
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "T1_US " << (t1_1 - t1_0) << "\nT1_CHK " << chk1 << "\n";
    std::cout << "T2_US " << (t2_1 - t2_0) << "\nT2_CHK " << chk2 << "\n";
    std::cout << "T3_US " << (t3_1 - t3_0) << "\nT3_CHK " << chk3 << "\n";
    std::cout << "T4_US " << (t4_1 - t4_0) << "\nT4_CHK " << chk4 << "\n";
    std::cout << "T5_US " << (t5_1 - t5_0) << "\nT5_CHK " << chk5 << "\n";
    std::cout << "TOTAL_US " << total_us << "\n";
    return 0;
}
