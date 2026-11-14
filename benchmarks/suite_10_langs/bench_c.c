#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

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

typedef struct {
    int64_t x;
    int64_t y;
    int64_t z;
} Node;

static int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

static inline int64_t step_fn(int64_t a, int64_t b) {
    int64_t next = a + b;
    if (next >= 1000003LL) next -= 1000003LL;
    return next;
}

typedef struct {
    int64_t start;
    int64_t end;
    int64_t cols;
    int64_t* row_sums;
} RowJob;

#ifdef _WIN32
static DWORD WINAPI row_worker(LPVOID arg) {
#else
static void* row_worker(void* arg) {
#endif
    RowJob* job = (RowJob*)arg;
    int64_t cols = job->cols;
    for (int64_t r = job->start; r < job->end; r++) {
        int64_t acc = r;
        for (int64_t c = 0; c < cols; c++) {
            acc += 7919LL;
            if (acc >= 1000003LL) acc -= 1000003LL;
        }
        job->row_sums[r] = acc;
    }
    return 0;
}

int main(void) {
    volatile int64_t v_n1 = 100000000LL;
    volatile float v_step1 = 0.00000001f;
    volatile int64_t v_n2 = 50000000LL;
    volatile int64_t v_n3 = 1000000LL;
    volatile int64_t v_f40 = 40LL;
    volatile int64_t v_n4 = 100000000LL;
    volatile int64_t v_rows5 = 2000LL;
    volatile int64_t v_cols5 = 50000LL;

    int64_t t_all_0 = time_us();

    /* Task 1 */
    int64_t n1 = v_n1;
    float step1 = v_step1;
    float sum1 = 0.0f;
    float x1 = 0.000000005f;
    int64_t t1_0 = time_us();
    for (int64_t i = 0; i < n1; i++) {
        sum1 = sum1 + 4.0f / (1.0f + x1 * x1);
        x1 = x1 + step1;
    }
    int64_t t1_1 = time_us();
    float pi1 = sum1 * step1;
    int64_t us1 = t1_1 - t1_0;

    /* Task 2 */
    int64_t n2 = v_n2;
    int64_t t2_0 = time_us();
    int64_t* arr2 = (int64_t*)calloc((size_t)n2, sizeof(int64_t));
    int64_t cur2 = 1;
    for (int64_t i = 0; i < n2; i++) {
        cur2 += 7;
        if (cur2 >= 1024) cur2 -= 1024;
        arr2[i] = cur2;
    }
    int64_t sum2 = 0;
    for (int64_t i = 0; i < n2; i++) {
        int64_t v = arr2[i] * 3 + 1;
        arr2[i] = v;
        sum2 += v;
    }
    free(arr2);
    int64_t t2_1 = time_us();
    int64_t us2 = t2_1 - t2_0;

    /* Task 3 */
    int64_t n3 = v_n3;
    int64_t t3_0 = time_us();
    Node** nodes3 = (Node**)malloc((size_t)n3 * sizeof(Node*));
    for (int64_t i = 0; i < n3; i++) {
        Node* p = (Node*)malloc(sizeof(Node));
        p->x = i;
        p->y = i * 2;
        p->z = i + 5;
        nodes3[i] = p;
    }
    int64_t sum3 = 0;
    for (int64_t i = 0; i < n3; i++) {
        Node* p = nodes3[i];
        sum3 += p->x + p->y + p->z;
    }
    for (int64_t i = 0; i < n3; i++) free(nodes3[i]);
    free(nodes3);
    int64_t t3_1 = time_us();
    int64_t us3 = t3_1 - t3_0;

    /* Task 4 */
    int64_t f_arg = v_f40;
    int64_t n4 = v_n4;
    int64_t t4_0 = time_us();
    int64_t f40 = fib(f_arg);
    int64_t acc4 = 0;
    for (int64_t i = 0; i < n4; i++) {
        acc4 = step_fn(acc4, 7919LL);
    }
    int64_t t4_1 = time_us();
    int64_t chk4 = f40 + acc4;
    int64_t us4 = t4_1 - t4_0;

    /* Task 5 */
    int64_t rows5 = v_rows5;
    int64_t cols5 = v_cols5;
    int64_t t5_0 = time_us();
    int64_t* row_sums = (int64_t*)calloc((size_t)rows5, sizeof(int64_t));
    int workers = cpu_count();
    RowJob jobs[64];
#ifdef _WIN32
    HANDLE threads[64];
#else
    pthread_t threads[64];
#endif
    int64_t chunk = rows5 / workers;
    int64_t rem = rows5 % workers;
    int64_t cur = 0;
    for (int w = 0; w < workers; w++) {
        int64_t sz = chunk + (w < rem ? 1 : 0);
        jobs[w].start = cur;
        jobs[w].end = cur + sz;
        jobs[w].cols = cols5;
        jobs[w].row_sums = row_sums;
        cur += sz;
#ifdef _WIN32
        threads[w] = CreateThread(NULL, 0, row_worker, &jobs[w], 0, NULL);
#else
        pthread_create(&threads[w], NULL, row_worker, &jobs[w]);
#endif
    }
#ifdef _WIN32
    WaitForMultipleObjects((DWORD)workers, threads, TRUE, INFINITE);
    for (int w = 0; w < workers; w++) CloseHandle(threads[w]);
#else
    for (int w = 0; w < workers; w++) pthread_join(threads[w], NULL);
#endif
    int64_t sum5 = 0;
    for (int64_t r = 0; r < rows5; r++) sum5 += row_sums[r];
    free(row_sums);
    int64_t t5_1 = time_us();
    int64_t us5 = t5_1 - t5_0;

    int64_t us_total = time_us() - t_all_0;

    printf("TASK1_US %lld\nTASK1_PI %.6f\nTASK2_US %lld\nTASK2_CHK %lld\n",
           (long long)us1, pi1, (long long)us2, (long long)sum2);
    printf("TASK3_US %lld\nTASK3_CHK %lld\nTASK4_US %lld\nTASK4_CHK %lld\n",
           (long long)us3, (long long)sum3, (long long)us4, (long long)chk4);
    printf("TASK5_US %lld\nTASK5_CHK %lld\nTOTAL_US %lld\n",
           (long long)us5, (long long)sum5, (long long)us_total);
    return 0;
}
