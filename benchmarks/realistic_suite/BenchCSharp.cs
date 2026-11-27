using System;
using System.Diagnostics;
using System.Globalization;
using System.Threading.Tasks;

class BenchCSharp {
    static void QSortI64(long[] arr, int lo, int hi) {
        int i = lo, j = hi;
        long pivot = arr[(lo + hi) >> 1];
        while (i <= j) {
            while (arr[i] < pivot) i++;
            while (arr[j] > pivot) j--;
            if (i <= j) {
                long tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
                i++;
                j--;
            }
        }
        if (lo < j) QSortI64(arr, lo, j);
        if (i < hi) QSortI64(arr, i, hi);
    }

    static long TicksToUs(long ticks) {
        return (ticks * 1000000L) / Stopwatch.Frequency;
    }

    static void Main() {
        const int n1 = 600, steps1 = 25;
        const int n2 = 500000;
        const int n3 = 320;
        const int n4 = 10000000;
        const int w5 = 1024, iter5 = 256;

        long tAll0 = Stopwatch.GetTimestamp();

        // T1
        double[] x = new double[n1], y = new double[n1], z = new double[n1];
        double[] vx = new double[n1], vy = new double[n1], vz = new double[n1], m = new double[n1];
        for (int i = 0; i < n1; i++) {
            x[i] = ((i * 17) % 100) * 0.1 - 5.0;
            y[i] = ((i * 31) % 100) * 0.1 - 5.0;
            z[i] = ((i * 47) % 100) * 0.1 - 5.0;
            vx[i] = ((i % 7) - 3) * 0.01;
            vy[i] = (((i * 3) % 7) - 3) * 0.01;
            vz[i] = (((i * 5) % 7) - 3) * 0.01;
            m[i] = 0.5 + (i % 5) * 0.25;
        }
        const double dt = 0.01, soft = 0.01;
        long t1_0 = Stopwatch.GetTimestamp();
        for (int s = 0; s < steps1; s++) {
            for (int i = 0; i < n1; i++) {
                double fx = 0.0, fy = 0.0, fz = 0.0;
                double xi = x[i], yi = y[i], ziv = z[i];
                for (int j = 0; j < n1; j++) {
                    if (j != i) {
                        double dx = x[j] - xi;
                        double dy = y[j] - yi;
                        double dz = z[j] - ziv;
                        double d2 = dx * dx + dy * dy + dz * dz + soft;
                        double inv = 1.0 / Math.Sqrt(d2);
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
            for (int i = 0; i < n1; i++) {
                x[i] += vx[i] * dt;
                y[i] += vy[i] * dt;
                z[i] += vz[i] * dt;
            }
        }
        double chk1 = 0.0;
        for (int i = 0; i < n1; i++) {
            chk1 += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        }
        long us1 = TicksToUs(Stopwatch.GetTimestamp() - t1_0);

        // T2
        long[] arr2 = new long[n2];
        long lcg = 12345L;
        for (int i = 0; i < n2; i++) {
            lcg = (lcg * 48271L) % 2147483647L;
            arr2[i] = lcg;
        }
        long t2_0 = Stopwatch.GetTimestamp();
        QSortI64(arr2, 0, n2 - 1);
        long chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1];
        long us2 = TicksToUs(Stopwatch.GetTimestamp() - t2_0);

        // T3
        double[] a3 = new double[n3 * n3], b3 = new double[n3 * n3], c3 = new double[n3 * n3];
        for (int i = 0; i < n3; i++) {
            for (int k = 0; k < n3; k++) {
                a3[i * n3 + k] = ((i * 7 + k * 13) % 97) * 0.01;
                b3[i * n3 + k] = ((i * 11 + k * 5) % 89) * 0.01;
            }
        }
        long t3_0 = Stopwatch.GetTimestamp();
        for (int i = 0; i < n3; i++) {
            int coff = i * n3;
            for (int k = 0; k < n3; k++) {
                double aik = a3[coff + k];
                int boff = k * n3;
                for (int j = 0; j < n3; j++) {
                    c3[coff + j] += aik * b3[boff + j];
                }
            }
        }
        double chk3 = 0.0;
        for (int i = 0; i < n3 * n3; i++) chk3 += c3[i];
        long us3 = TicksToUs(Stopwatch.GetTimestamp() - t3_0);

        // T4
        int[] isComp = new int[n4];
        long t4_0 = Stopwatch.GetTimestamp();
        for (int p = 2; p * p < n4; p++) {
            if (isComp[p] == 0) {
                for (int m4 = p * p; m4 < n4; m4 += p) isComp[m4] = 1;
            }
        }
        long chk4 = 0;
        for (int i = 2; i < n4; i++) {
            if (isComp[i] == 0) chk4 += i;
        }
        long us4 = TicksToUs(Stopwatch.GetTimestamp() - t4_0);

        // T5
        long[] rowSums = new long[w5];
        long t5_0 = Stopwatch.GetTimestamp();
        Parallel.For(0, w5, py => {
            double cy = -1.25 + ((double)py * 2.5) / (double)w5;
            long rsum = 0;
            for (int px = 0; px < w5; px++) {
                double cx = -2.0 + ((double)px * 3.0) / (double)w5;
                double zr = 0.0, zi = 0.0;
                int it = 0;
                while (it < iter5 && (zr * zr + zi * zi <= 4.0)) {
                    double tmp = zr * zr - zi * zi + cx;
                    zi = 2.0 * zr * zi + cy;
                    zr = tmp;
                    it++;
                }
                rsum += it;
            }
            rowSums[py] = rsum;
        });
        long chk5 = 0;
        for (int i = 0; i < w5; i++) chk5 += rowSums[i];
        long us5 = TicksToUs(Stopwatch.GetTimestamp() - t5_0);

        long totalUs = TicksToUs(Stopwatch.GetTimestamp() - tAll0);
        Console.WriteLine("T1_US " + us1);
        Console.WriteLine("T1_CHK " + chk1.ToString("F6", CultureInfo.InvariantCulture));
        Console.WriteLine("T2_US " + us2);
        Console.WriteLine("T2_CHK " + chk2);
        Console.WriteLine("T3_US " + us3);
        Console.WriteLine("T3_CHK " + chk3.ToString("F6", CultureInfo.InvariantCulture));
        Console.WriteLine("T4_US " + us4);
        Console.WriteLine("T4_CHK " + chk4);
        Console.WriteLine("T5_US " + us5);
        Console.WriteLine("T5_CHK " + chk5);
        Console.WriteLine("TOTAL_US " + totalUs);
    }
}
