use std::hint::black_box;
use std::thread;
use std::time::Instant;

struct Node {
    x: i64,
    y: i64,
    z: i64,
}

fn fib(n: i64) -> i64 {
    if n <= 1 {
        return n;
    }
    fib(n - 1) + fib(n - 2)
}

#[inline(always)]
fn step_fn(a: i64, b: i64) -> i64 {
    let mut next = a + b;
    if next >= 1_000_003 {
        next -= 1_000_003;
    }
    next
}

fn main() {
    let t_all = Instant::now();

    // Task 1
    let n1: i64 = black_box(100_000_000);
    let step1: f32 = black_box(0.00000001_f32);
    let mut sum1: f32 = 0.0_f32;
    let mut x1: f32 = 0.000000005_f32;
    let t1 = Instant::now();
    for _ in 0..n1 {
        sum1 = sum1 + 4.0_f32 / (1.0_f32 + x1 * x1);
        x1 = x1 + step1;
    }
    let us1 = t1.elapsed().as_micros();
    let pi1: f32 = sum1 * step1;

    // Task 2
    let n2: usize = black_box(50_000_000);
    let t2 = Instant::now();
    let mut arr2 = vec![0_i64; n2];
    let mut cur2: i64 = 1;
    for slot in arr2.iter_mut() {
        cur2 += 7;
        if cur2 >= 1024 {
            cur2 -= 1024;
        }
        *slot = cur2;
    }
    let mut sum2: i64 = 0;
    for slot in arr2.iter_mut() {
        let v = *slot * 3 + 1;
        *slot = v;
        sum2 += v;
    }
    drop(arr2);
    let us2 = t2.elapsed().as_micros();

    // Task 3
    let n3: i64 = black_box(1_000_000);
    let t3 = Instant::now();
    let sum3: i64;
    {
        let mut nodes3: Vec<Box<Node>> = Vec::with_capacity(n3 as usize);
        for i in 0..n3 {
            nodes3.push(Box::new(Node {
                x: i,
                y: i * 2,
                z: i + 5,
            }));
        }
        let mut acc3: i64 = 0;
        for p in &nodes3 {
            acc3 += p.x + p.y + p.z;
        }
        sum3 = acc3;
    }
    let us3 = t3.elapsed().as_micros();

    // Task 4
    let f_arg: i64 = black_box(40);
    let n4: i64 = black_box(100_000_000);
    let t4 = Instant::now();
    let f40 = fib(f_arg);
    let mut acc4: i64 = 0;
    for _ in 0..n4 {
        acc4 = step_fn(acc4, 7919);
    }
    let us4 = t4.elapsed().as_micros();
    let chk4 = f40 + acc4;

    // Task 5
    let rows5: usize = black_box(2000);
    let cols5: i64 = black_box(50_000);
    let t5 = Instant::now();
    let mut row_sums = vec![0_i64; rows5];
    let workers = thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    let chunk = (rows5 + workers - 1) / workers;
    thread::scope(|s| {
        for (t_idx, slice) in row_sums.chunks_mut(chunk).enumerate() {
            let base_row = (t_idx * chunk) as i64;
            s.spawn(move || {
                for (offset, out) in slice.iter_mut().enumerate() {
                    let mut acc = base_row + offset as i64;
                    for _ in 0..cols5 {
                        acc += 7919;
                        if acc >= 1_000_003 {
                            acc -= 1_000_003;
                        }
                    }
                    *out = acc;
                }
            });
        }
    });
    let sum5: i64 = row_sums.iter().sum();
    let us5 = t5.elapsed().as_micros();

    let us_total = t_all.elapsed().as_micros();

    println!("TASK1_US {}", us1);
    println!("TASK1_PI {:.6}", pi1);
    println!("TASK2_US {}", us2);
    println!("TASK2_CHK {}", sum2);
    println!("TASK3_US {}", us3);
    println!("TASK3_CHK {}", sum3);
    println!("TASK4_US {}", us4);
    println!("TASK4_CHK {}", chk4);
    println!("TASK5_US {}", us5);
    println!("TASK5_CHK {}", sum5);
    println!("TOTAL_US {}", us_total);
}
