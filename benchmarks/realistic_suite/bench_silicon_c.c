#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

int main(void) {
    volatile int64_t vd512 = 512, vd1024 = 1024;
    int64_t d512 = vd512, d1024 = vd1024;

    double* a512 = (double*)malloc((size_t)(d512 * d512) * sizeof(double));
    double* b512 = (double*)malloc((size_t)(d512 * d512) * sizeof(double));
    double* c512 = (double*)calloc((size_t)(d512 * d512), sizeof(double));
    for (int64_t i = 0; i < d512 * d512; i++) {
        a512[i] = (double)((i % 13) + 1) * 0.025;
        b512[i] = (double)((i % 17) + 1) * 0.025;
    }
    int64_t t0 = time_us();
    for (int64_t i = 0; i < d512; i++) {
        double* crow = &c512[i * d512];
        for (int64_t k = 0; k < d512; k++) {
            double aik = a512[i * d512 + k];
            const double* brow = &b512[k * d512];
            for (int64_t j = 0; j < d512; j++) crow[j] += aik * brow[j];
        }
    }
    int64_t us_f64_512 = time_us() - t0;
    double chk512 = 0.0;
    for (int64_t i = 0; i < d512 * d512; i++) chk512 += c512[i];

    float* fa = (float*)malloc((size_t)(d1024 * d1024) * sizeof(float));
    float* fb = (float*)malloc((size_t)(d1024 * d1024) * sizeof(float));
    float* fc = (float*)calloc((size_t)(d1024 * d1024), sizeof(float));
    for (int64_t i = 0; i < d1024 * d1024; i++) {
        fa[i] = (float)(i % 17) * 0.015f;
        fb[i] = (float)(i % 13) * 0.015f;
    }
    t0 = time_us();
    for (int64_t i = 0; i < d1024; i++) {
        float* crow = &fc[i * d1024];
        for (int64_t k = 0; k < d1024; k++) {
            float aik = fa[i * d1024 + k];
            const float* brow = &fb[k * d1024];
            for (int64_t j = 0; j < d1024; j++) crow[j] += aik * brow[j];
        }
    }
    int64_t us_f32_1024 = time_us() - t0;
    double chk1024 = 0.0;
    for (int64_t i = 0; i < d1024 * d1024; i++) chk1024 += (double)fc[i];

    printf("C_F64_512_US %lld\n", (long long)us_f64_512);
    printf("C_F64_512_CHK %.6f\n", chk512);
    printf("C_F32_1024_US %lld\n", (long long)us_f32_1024);
    printf("C_F32_1024_CHK %.2f\n", chk1024);

    free(a512); free(b512); free(c512);
    free(fa); free(fb); free(fc);
    return 0;
}
