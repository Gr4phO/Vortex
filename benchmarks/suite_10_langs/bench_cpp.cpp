#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

struct Node {
    int64_t x;
    int64_t y;
    int64_t z;
};

static int64_t time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

static inline int64_t step_fn(int64_t a, int64_t b) {
    int64_t next = a + b;
    if (next >= 1000003LL) next -= 1000003LL;
    return next;
}

int main() {
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
    std::vector<int64_t> arr2(static_cast<size_t>(n2), 0);
    int64_t cur2 = 1;
    for (int64_t i = 0; i < n2; i++) {
        cur2 += 7;
        if (cur2 >= 1024) cur2 -= 1024;
        arr2[static_cast<size_t>(i)] = cur2;
    }
    int64_t sum2 = 0;
    for (int64_t i = 0; i < n2; i++) {
        int64_t v = arr2[static_cast<size_t>(i)] * 3 + 1;
        arr2[static_cast<size_t>(i)] = v;
        sum2 += v;
    }
    arr2.clear();
    arr2.shrink_to_fit();
    int64_t t2_1 = time_us();
    int64_t us2 = t2_1 - t2_0;

    /* Task 3 */
    int64_t n3 = v_n3;
    int64_t t3_0 = time_us();
    {
        std::vector<std::unique_ptr<Node>> nodes3;
        nodes3.reserve(static_cast<size_t>(n3));
        for (int64_t i = 0; i < n3; i++) {
            nodes3.push_back(std::make_unique<Node>(Node{i, i * 2, i + 5}));
        }
        int64_t sum3 = 0;
        for (int64_t i = 0; i < n3; i++) {
            const Node* p = nodes3[static_cast<size_t>(i)].get();
            sum3 += p->x + p->y + p->z;
        }
        volatile int64_t keep3 = sum3;
        (void)keep3;
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
        std::vector<int64_t> row_sums(static_cast<size_t>(rows5), 0);
        unsigned workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 1;
        std::vector<std::thread> pool;
        pool.reserve(workers);
        int64_t chunk = rows5 / workers;
        int64_t rem = rows5 % workers;
        int64_t cur = 0;
        for (unsigned w = 0; w < workers; w++) {
            int64_t sz = chunk + (static_cast<int64_t>(w) < rem ? 1 : 0);
            int64_t start = cur;
            int64_t end = cur + sz;
            cur = end;
            pool.emplace_back([start, end, cols5, &row_sums]() {
                for (int64_t r = start; r < end; r++) {
                    int64_t acc = r;
                    for (int64_t c = 0; c < cols5; c++) {
                        acc += 7919LL;
                        if (acc >= 1000003LL) acc -= 1000003LL;
                    }
                    row_sums[static_cast<size_t>(r)] = acc;
                }
            });
        }
        for (auto& th : pool) th.join();
        int64_t sum5 = 0;
        for (int64_t r = 0; r < rows5; r++) sum5 += row_sums[static_cast<size_t>(r)];
        int64_t t5_1 = time_us();
        int64_t us5 = t5_1 - t5_0;
        int64_t us_total = time_us() - t_all_0;

        std::printf("TASK1_US %lld\nTASK1_PI %.6f\nTASK2_US %lld\nTASK2_CHK %lld\n",
                    (long long)us1, pi1, (long long)us2, (long long)sum2);
        std::printf("TASK3_US %lld\nTASK3_CHK %lld\nTASK4_US %lld\nTASK4_CHK %lld\n",
                    (long long)us3, (long long)sum3, (long long)us4, (long long)chk4);
        std::printf("TASK5_US %lld\nTASK5_CHK %lld\nTOTAL_US %lld\n",
                    (long long)us5, (long long)sum5, (long long)us_total);
    }
    return 0;
}
