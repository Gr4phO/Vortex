#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int64_t time_us(void) {
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (int64_t)((cnt.QuadPart * 1000000LL) / freq.QuadPart);
}
static int cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    return (n > 0 && n <= 64) ? n : 1;
}
#else
#include <time.h>
#include <unistd.h>
#include <pthread.h>
static int64_t time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
}
static int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0 && n <= 64) ? (int)n : 1;
}
#endif

static void qsort_i64(int64_t* arr, int64_t lo, int64_t hi) {
    int64_t i = lo;
    int64_t j = hi;
    int64_t pivot = arr[(lo + hi) / 2];
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) {
            int64_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
            i++;
            j--;
        }
    }
    if (lo < j) qsort_i64(arr, lo, j);
    if (i < hi) qsort_i64(arr, i, hi);
}

typedef struct {
    int64_t start;
    int64_t end;
    int64_t w;
    int64_t max_iter;
    int64_t* row_sums;
} MandelJob;

#ifdef _WIN32
static DWORD WINAPI mandel_worker(LPVOID arg) {
#else
static void* mandel_worker(void* arg) {
#endif
    MandelJob* job = (MandelJob*)arg;
    int64_t W = job->w;
    int64_t max_iter = job->max_iter;
    for (int64_t py = job->start; py < job->end; py++) {
        double cy = -1.25 + ((double)py * 2.5) / (double)W;
        int64_t row_sum = 0;
        for (int64_t px = 0; px < W; px++) {
            double cx = -2.0 + ((double)px * 3.0) / (double)W;
            double zr = 0.0, zi = 0.0;
            int64_t it = 0;
            while (it < max_iter && (zr * zr + zi * zi <= 4.0)) {
                double tmp = zr * zr - zi * zi + cx;
                zi = 2.0 * zr * zi + cy;
                zr = tmp;
                it++;
            }
            row_sum += it;
        }
        job->row_sums[py] = row_sum;
    }
    return 0;
}

int main(void) {
    volatile int64_t v_n1 = 600, v_steps1 = 25;
    volatile int64_t v_n2 = 500000;
    volatile int64_t v_n3 = 320;
    volatile int64_t v_n4 = 10000000;
    volatile int64_t v_w5 = 1024, v_iter5 = 256;

    int64_t t_all0 = time_us();

    /* T1: N-Body Gravity f64 */
    int64_t n1 = v_n1, steps1 = v_steps1;
    double* x = (double*)malloc((size_t)n1 * sizeof(double));
    double* y = (double*)malloc((size_t)n1 * sizeof(double));
    double* z = (double*)malloc((size_t)n1 * sizeof(double));
    double* vx = (double*)malloc((size_t)n1 * sizeof(double));
    double* vy = (double*)malloc((size_t)n1 * sizeof(double));
    double* vz = (double*)malloc((size_t)n1 * sizeof(double));
    double* m = (double*)malloc((size_t)n1 * sizeof(double));
    for (int64_t i = 0; i < n1; i++) {
        x[i] = (double)((i * 17) % 100) * 0.1 - 5.0;
        y[i] = (double)((i * 31) % 100) * 0.1 - 5.0;
        z[i] = (double)((i * 47) % 100) * 0.1 - 5.0;
        vx[i] = (double)((i % 7) - 3) * 0.01;
        vy[i] = (double)(((i * 3) % 7) - 3) * 0.01;
        vz[i] = (double)(((i * 5) % 7) - 3) * 0.01;
        m[i] = 0.5 + (double)(i % 5) * 0.25;
    }
    double dt = 0.01, soft = 0.01;
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
                    double inv = 1.0 / sqrt(d2);
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
    free(x); free(y); free(z); free(vx); free(vy); free(vz); free(m);

    /* T2: Quicksort 500k i64 */
    int64_t n2 = v_n2;
    int64_t* arr2 = (int64_t*)malloc((size_t)n2 * sizeof(int64_t));
    int64_t lcg = 12345;
    for (int64_t i = 0; i < n2; i++) {
        lcg = (lcg * 48271LL) % 2147483647LL;
        arr2[i] = lcg;
    }
    int64_t t2_0 = time_us();
    qsort_i64(arr2, 0, n2 - 1);
    int64_t chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1];
    int64_t t2_1 = time_us();
    free(arr2);

    /* T3: Dense Matrix Multiply f64 320x320x320 (pure i-k-j) */
    int64_t n3 = v_n3;
    double* a3 = (double*)malloc((size_t)(n3 * n3) * sizeof(double));
    double* b3 = (double*)malloc((size_t)(n3 * n3) * sizeof(double));
    double* c3 = (double*)calloc((size_t)(n3 * n3), sizeof(double));
    for (int64_t i = 0; i < n3; i++) {
        for (int64_t k = 0; k < n3; k++) {
            a3[i * n3 + k] = (double)((i * 7 + k * 13) % 97) * 0.01;
            b3[i * n3 + k] = (double)((i * 11 + k * 5) % 89) * 0.01;
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
    free(a3); free(b3); free(c3);

    /* T4: Sieve of Eratosthenes N = 10,000,000 */
    int64_t n4 = v_n4;
    int32_t* is_comp = (int32_t*)calloc((size_t)n4, sizeof(int32_t));
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
    free(is_comp);

    /* T5: Multi-Core Parallel Mandelbrot 1024x1024 */
    int64_t w5 = v_w5, iter5 = v_iter5;
    int64_t* row_sums = (int64_t*)calloc((size_t)w5, sizeof(int64_t));
    int workers = cpu_count();
    MandelJob jobs[64];
#ifdef _WIN32
    HANDLE threads[64];
#else
    pthread_t threads[64];
#endif
    int64_t t5_0 = time_us();
    int64_t chunk = w5 / workers, rem = w5 % workers, cur = 0;
    for (int w = 0; w < workers; w++) {
        int64_t sz = chunk + (w < rem ? 1 : 0);
        jobs[w].start = cur;
        jobs[w].end = cur + sz;
        jobs[w].w = w5;
        jobs[w].max_iter = iter5;
        jobs[w].row_sums = row_sums;
        cur += sz;
#ifdef _WIN32
        threads[w] = CreateThread(NULL, 0, mandel_worker, &jobs[w], 0, NULL);
#else
        pthread_create(&threads[w], NULL, mandel_worker, &jobs[w]);
#endif
    }
#ifdef _WIN32
    WaitForMultipleObjects((DWORD)workers, threads, TRUE, INFINITE);
    for (int w = 0; w < workers; w++) CloseHandle(threads[w]);
#else
    for (int w = 0; w < workers; w++) pthread_join(threads[w], NULL);
#endif
    int64_t chk5 = 0;
    for (int64_t i = 0; i < w5; i++) chk5 += row_sums[i];
    int64_t t5_1 = time_us();
    free(row_sums);

    int64_t total_us = time_us() - t_all0;
    printf("T1_US %lld\nT1_CHK %.6f\n", (long long)(t1_1 - t1_0), chk1);
    printf("T2_US %lld\nT2_CHK %lld\n", (long long)(t2_1 - t2_0), (long long)chk2);
    printf("T3_US %lld\nT3_CHK %.6f\n", (long long)(t3_1 - t3_0), chk3);
    printf("T4_US %lld\nT4_CHK %lld\n", (long long)(t4_1 - t4_0), (long long)chk4);
    printf("T5_US %lld\nT5_CHK %lld\n", (long long)(t5_1 - t5_0), (long long)chk5);
    printf("TOTAL_US %lld\n", (long long)total_us);
    return 0;
}
