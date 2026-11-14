using System;
using System.Diagnostics;
using System.Globalization;
using System.Threading.Tasks;

public sealed class Node
{
    public long X;
    public long Y;
    public long Z;
}

public class BenchCSharp
{
    static long Fib(long n)
    {
        if (n <= 1) return n;
        return Fib(n - 1) + Fib(n - 2);
    }

    static long StepFn(long a, long b)
    {
        long next = a + b;
        if (next >= 1000003L) next -= 1000003L;
        return next;
    }

    static long ToUs(long ticks)
    {
        return (ticks * 1000000L) / Stopwatch.Frequency;
    }

    public static void Main(string[] args)
    {
        long tAll = Stopwatch.GetTimestamp();

        // Task 1
        const long n1 = 100000000L;
        float step1 = 0.00000001f;
        float sum1 = 0.0f;
        float x1 = 0.000000005f;
        long t1_0 = Stopwatch.GetTimestamp();
        for (long i = 0; i < n1; i++)
        {
            sum1 = sum1 + 4.0f / (1.0f + x1 * x1);
            x1 = x1 + step1;
        }
        long us1 = ToUs(Stopwatch.GetTimestamp() - t1_0);
        float pi1 = sum1 * step1;

        // Task 2
        const int n2 = 50000000;
        long t2_0 = Stopwatch.GetTimestamp();
        long[] arr2 = new long[n2];
        long cur2 = 1;
        for (int i = 0; i < n2; i++)
        {
            cur2 += 7;
            if (cur2 >= 1024) cur2 -= 1024;
            arr2[i] = cur2;
        }
        long sum2 = 0;
        for (int i = 0; i < n2; i++)
        {
            long v = arr2[i] * 3 + 1;
            arr2[i] = v;
            sum2 += v;
        }
        arr2 = null;
        long us2 = ToUs(Stopwatch.GetTimestamp() - t2_0);

        // Task 3
        const int n3 = 1000000;
        long t3_0 = Stopwatch.GetTimestamp();
        Node[] nodes3 = new Node[n3];
        for (int i = 0; i < n3; i++)
        {
            nodes3[i] = new Node { X = i, Y = (long)i * 2L, Z = (long)i + 5L };
        }
        long sum3 = 0;
        for (int i = 0; i < n3; i++)
        {
            Node p = nodes3[i];
            sum3 += p.X + p.Y + p.Z;
        }
        nodes3 = null;
        long us3 = ToUs(Stopwatch.GetTimestamp() - t3_0);

        // Task 4
        long t4_0 = Stopwatch.GetTimestamp();
        long f40 = Fib(40);
        long acc4 = 0;
        for (long i = 0; i < 100000000L; i++)
        {
            acc4 = StepFn(acc4, 7919L);
        }
        long us4 = ToUs(Stopwatch.GetTimestamp() - t4_0);
        long chk4 = f40 + acc4;

        // Task 5
        const int rows5 = 2000;
        const int cols5 = 50000;
        long t5_0 = Stopwatch.GetTimestamp();
        long[] rowSums = new long[rows5];
        Parallel.For(0, rows5, r =>
        {
            long acc = r;
            for (int c = 0; c < cols5; c++)
            {
                acc += 7919L;
                if (acc >= 1000003L) acc -= 1000003L;
            }
            rowSums[r] = acc;
        });
        long sum5 = 0;
        for (int r = 0; r < rows5; r++) sum5 += rowSums[r];
        long us5 = ToUs(Stopwatch.GetTimestamp() - t5_0);

        long usTotal = ToUs(Stopwatch.GetTimestamp() - tAll);

        Console.WriteLine(string.Format(CultureInfo.InvariantCulture, "TASK1_US {0}", us1));
        Console.WriteLine(string.Format(CultureInfo.InvariantCulture, "TASK1_PI {0:F6}", pi1));
        Console.WriteLine("TASK2_US {0}", us2);
        Console.WriteLine("TASK2_CHK {0}", sum2);
        Console.WriteLine("TASK3_US {0}", us3);
        Console.WriteLine("TASK3_CHK {0}", sum3);
        Console.WriteLine("TASK4_US {0}", us4);
        Console.WriteLine("TASK4_CHK {0}", chk4);
        Console.WriteLine("TASK5_US {0}", us5);
        Console.WriteLine("TASK5_CHK {0}", sum5);
        Console.WriteLine("TOTAL_US {0}", usTotal);
    }
}
