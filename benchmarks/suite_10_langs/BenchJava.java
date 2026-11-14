import java.util.Arrays;

public final class BenchJava {
    static final class Node {
        long x;
        long y;
        long z;
        Node(long x, long y, long z) {
            this.x = x;
            this.y = y;
            this.z = z;
        }
    }

    static long fib(long n) {
        if (n <= 1) return n;
        return fib(n - 1) + fib(n - 2);
    }

    static long stepFn(long a, long b) {
        long next = a + b;
        if (next >= 1000003L) next -= 1000003L;
        return next;
    }

    public static void main(String[] args) throws Exception {
        long tAll = System.nanoTime();

        // Task 1
        final long reps1 = 100L;
        final long n1 = 1_000_000L;
        final float step1 = 0.000001f;
        float totalPi1 = 0.0f;
        long t1_0 = System.nanoTime();
        for (long r = 0; r < reps1; r++) {
            float sum1 = 0.0f;
            float x1 = 0.0000005f;
            for (long i = 0; i < n1; i++) {
                sum1 = sum1 + 4.0f / (1.0f + x1 * x1);
                x1 = x1 + step1;
            }
            totalPi1 += sum1 * step1;
        }
        long us1 = (System.nanoTime() - t1_0) / 1000L;
        float pi1 = totalPi1 * 0.01f;

        // Task 2
        final int n2 = 50_000_000;
        long t2_0 = System.nanoTime();
        long[] arr2 = new long[n2];
        long cur2 = 1;
        for (int i = 0; i < n2; i++) {
            cur2 += 7;
            if (cur2 >= 1024) cur2 -= 1024;
            arr2[i] = cur2;
        }
        long sum2 = 0;
        for (int i = 0; i < n2; i++) {
            long v = arr2[i] * 3 + 1;
            arr2[i] = v;
            sum2 += v;
        }
        arr2 = null;
        long us2 = (System.nanoTime() - t2_0) / 1000L;

        // Task 3
        final int n3 = 1_000_000;
        long t3_0 = System.nanoTime();
        Node[] nodes3 = new Node[n3];
        for (int i = 0; i < n3; i++) {
            nodes3[i] = new Node(i, (long) i * 2L, (long) i + 5L);
        }
        long sum3 = 0;
        for (int i = 0; i < n3; i++) {
            Node p = nodes3[i];
            sum3 += p.x + p.y + p.z;
        }
        nodes3 = null;
        long us3 = (System.nanoTime() - t3_0) / 1000L;

        // Task 4
        long t4_0 = System.nanoTime();
        long f40 = fib(40L);
        long acc4 = 0;
        for (long i = 0; i < 100_000_000L; i++) {
            acc4 = stepFn(acc4, 7919L);
        }
        long us4 = (System.nanoTime() - t4_0) / 1000L;
        long chk4 = f40 + acc4;

        // Task 5
        final int rows5 = 2000;
        final int cols5 = 50000;
        long t5_0 = System.nanoTime();
        final long[] rowSums = new long[rows5];
        int workers = Runtime.getRuntime().availableProcessors();
        Thread[] threads = new Thread[workers];
        int chunk = rows5 / workers;
        int rem = rows5 % workers;
        int cur = 0;
        for (int w = 0; w < workers; w++) {
            final int sz = chunk + (w < rem ? 1 : 0);
            final int start = cur;
            final int end = cur + sz;
            cur = end;
            threads[w] = new Thread(() -> {
                for (int r = start; r < end; r++) {
                    long acc = r;
                    for (int c = 0; c < cols5; c++) {
                        acc += 7919L;
                        if (acc >= 1000003L) acc -= 1000003L;
                    }
                    rowSums[r] = acc;
                }
            });
            threads[w].start();
        }
        for (Thread th : threads) th.join();
        long sum5 = 0;
        for (int r = 0; r < rows5; r++) sum5 += rowSums[r];
        long us5 = (System.nanoTime() - t5_0) / 1000L;

        long usTotal = (System.nanoTime() - tAll) / 1000L;

        System.out.printf("TASK1_US %d%nTASK1_PI %.6f%nTASK2_US %d%nTASK2_CHK %d%n", us1, pi1, us2, sum2);
        System.out.printf("TASK3_US %d%nTASK3_CHK %d%nTASK4_US %d%nTASK4_CHK %d%n", us3, sum3, us4, chk4);
        System.out.printf("TASK5_US %d%nTASK5_CHK %d%nTOTAL_US %d%n", us5, sum5, usTotal);
    }
}
