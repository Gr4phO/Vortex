import java.util.Locale;
import java.util.stream.IntStream;

public class BenchJava {
    static void qsortI64(long[] arr, int lo, int hi) {
        int i = lo, j = hi;
        long pivot = arr[(lo + hi) >>> 1];
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
        if (lo < j) qsortI64(arr, lo, j);
        if (i < hi) qsortI64(arr, i, hi);
    }

    public static void main(String[] args) {
        Locale.setDefault(Locale.US);
        final int n1 = 600, steps1 = 25;
        final int n2 = 500000;
        final int n3 = 320;
        final int n4 = 10000000;
        final int w5 = 1024, iter5 = 256;

        long tAll0 = System.nanoTime();

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
        final double dt = 0.01, soft = 0.01;
        long t1_0 = System.nanoTime();
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
                        double inv = 1.0 / Math.sqrt(d2);
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
        long us1 = (System.nanoTime() - t1_0) / 1000L;

        // T2
        long[] arr2 = new long[n2];
        long lcg = 12345L;
        for (int i = 0; i < n2; i++) {
            lcg = (lcg * 48271L) % 2147483647L;
            arr2[i] = lcg;
        }
        long t2_0 = System.nanoTime();
        qsortI64(arr2, 0, n2 - 1);
        long chk2 = arr2[0] + arr2[250000] + arr2[n2 - 1];
        long us2 = (System.nanoTime() - t2_0) / 1000L;

        // T3
        double[] a3 = new double[n3 * n3], b3 = new double[n3 * n3], c3 = new double[n3 * n3];
        for (int i = 0; i < n3; i++) {
            for (int k = 0; k < n3; k++) {
                a3[i * n3 + k] = ((i * 7 + k * 13) % 97) * 0.01;
                b3[i * n3 + k] = ((i * 11 + k * 5) % 89) * 0.01;
            }
        }
        long t3_0 = System.nanoTime();
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
        long us3 = (System.nanoTime() - t3_0) / 1000L;

        // T4
        int[] isComp = new int[n4];
        long t4_0 = System.nanoTime();
        for (int p = 2; p * p < n4; p++) {
            if (isComp[p] == 0) {
                for (int m4 = p * p; m4 < n4; m4 += p) isComp[m4] = 1;
            }
        }
        long chk4 = 0;
        for (int i = 2; i < n4; i++) {
            if (isComp[i] == 0) chk4 += i;
        }
        long us4 = (System.nanoTime() - t4_0) / 1000L;

        // T5
        long[] rowSums = new long[w5];
        long t5_0 = System.nanoTime();
        IntStream.range(0, w5).parallel().forEach(py -> {
            double cy = -1.25 + ((double) py * 2.5) / (double) w5;
            long rsum = 0;
            for (int px = 0; px < w5; px++) {
                double cx = -2.0 + ((double) px * 3.0) / (double) w5;
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
        long us5 = (System.nanoTime() - t5_0) / 1000L;

        long totalUs = (System.nanoTime() - tAll0) / 1000L;
        System.out.printf("T1_US %d%nT1_CHK %.6f%n", us1, chk1);
        System.out.printf("T2_US %d%nT2_CHK %d%n", us2, chk2);
        System.out.printf("T3_US %d%nT3_CHK %.6f%n", us3, chk3);
        System.out.printf("T4_US %d%nT4_CHK %d%n", us4, chk4);
        System.out.printf("T5_US %d%nT5_CHK %d%n", us5, chk5);
        System.out.printf("TOTAL_US %d%n", totalUs);
    }
}
