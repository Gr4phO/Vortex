use std::hint::black_box;
use std::time::Instant;

fn qsort_i64(arr: &mut [i64], lo: isize, hi: isize) {
    let mut i = lo;
    let mut j = hi;
    let pivot = arr[((lo + hi) / 2) as usize];
    while i <= j {
        while arr[i as usize] < pivot {
            i += 1;
        }
        while arr[j as usize] > pivot {
            j -= 1;
        }
        if i <= j {
            arr.swap(i as usize, j as usize);
            i += 1;
            j -= 1;
        }
    }
    if lo < j {
        qsort_i64(arr, lo, j);
    }
    if i < hi {
        qsort_i64(arr, i, hi);
    }
}

fn main() {
    let n1 = black_box(600usize);
    let steps1 = black_box(25usize);
    let n2 = black_box(500_000usize);
    let n3 = black_box(320usize);
    let n4 = black_box(10_000_000usize);
    let w5 = black_box(1024usize);
    let iter5 = black_box(256i64);

    let t_all0 = Instant::now();

    // T1: N-Body Gravity f64
    let mut x = vec![0.0f64; n1];
    let mut y = vec![0.0f64; n1];
    let mut z = vec![0.0f64; n1];
    let mut vx = vec![0.0f64; n1];
    let mut vy = vec![0.0f64; n1];
    let mut vz = vec![0.0f64; n1];
    let mut m = vec![0.0f64; n1];
    for i in 0..n1 {
        x[i] = ((i * 17) % 100) as f64 * 0.1 - 5.0;
        y[i] = ((i * 31) % 100) as f64 * 0.1 - 5.0;
        z[i] = ((i * 47) % 100) as f64 * 0.1 - 5.0;
        vx[i] = ((i % 7) as isize - 3) as f64 * 0.01;
        vy[i] = (((i * 3) % 7) as isize - 3) as f64 * 0.01;
        vz[i] = (((i * 5) % 7) as isize - 3) as f64 * 0.01;
        m[i] = 0.5 + (i % 5) as f64 * 0.25;
    }
    let dt = 0.01f64;
    let soft = 0.01f64;
    let t1_0 = Instant::now();
    for _ in 0..steps1 {
        for i in 0..n1 {
            let mut fx = 0.0f64;
            let mut fy = 0.0f64;
            let mut fz = 0.0f64;
            let xi = x[i];
            let yi = y[i];
            let zi_v = z[i];
            for j in 0..n1 {
                if j != i {
                    let dx = x[j] - xi;
                    let dy = y[j] - yi;
                    let dz = z[j] - zi_v;
                    let d2 = dx * dx + dy * dy + dz * dz + soft;
                    let inv = 1.0 / d2.sqrt();
                    let f = m[j] * inv * inv * inv;
                    fx += dx * f;
                    fy += dy * f;
                    fz += dz * f;
                }
            }
            vx[i] += fx * dt;
            vy[i] += fy * dt;
            vz[i] += fz * dt;
        }
        for i in 0..n1 {
            x[i] += vx[i] * dt;
            y[i] += vy[i] * dt;
            z[i] += vz[i] * dt;
        }
    }
    let mut chk1 = 0.0f64;
    for i in 0..n1 {
        chk1 += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    }
    let us1 = t1_0.elapsed().as_micros();

    // T2: Quicksort 500k i64
    let mut arr2 = vec![0i64; n2];
    let mut lcg: i64 = 12345;
    for i in 0..n2 {
        lcg = (lcg * 48271) % 2147483647;
        arr2[i] = lcg;
    }
    let t2_0 = Instant::now();
    qsort_i64(&mut arr2, 0, (n2 - 1) as isize);
    let chk2 = arr2[0] + arr2[250_000] + arr2[n2 - 1];
    let us2 = t2_0.elapsed().as_micros();

    // T3: Matmul f64 320x320x320 (pure i-k-j)
    let mut a3 = vec![0.0f64; n3 * n3];
    let mut b3 = vec![0.0f64; n3 * n3];
    let mut c3 = vec![0.0f64; n3 * n3];
    for i in 0..n3 {
        for k in 0..n3 {
            a3[i * n3 + k] = ((i * 7 + k * 13) % 97) as f64 * 0.01;
            b3[i * n3 + k] = ((i * 11 + k * 5) % 89) as f64 * 0.01;
        }
    }
    let t3_0 = Instant::now();
    for i in 0..n3 {
        let crow = &mut c3[i * n3..(i + 1) * n3];
        for k in 0..n3 {
            let aik = a3[i * n3 + k];
            let brow = &b3[k * n3..(k + 1) * n3];
            for (c, &b) in crow.iter_mut().zip(brow.iter()) {
                *c += aik * b;
            }
        }
    }
    let mut chk3 = 0.0f64;
    for &v in &c3 {
        chk3 += v;
    }
    let us3 = t3_0.elapsed().as_micros();

    // T4: Sieve of Eratosthenes 10M
    let mut is_comp = vec![0i32; n4];
    let t4_0 = Instant::now();
    let mut p = 2usize;
    while p * p < n4 {
        if is_comp[p] == 0 {
            let mut m4 = p * p;
            while m4 < n4 {
                is_comp[m4] = 1;
                m4 += p;
            }
        }
        p += 1;
    }
    let mut chk4: i64 = 0;
    for i in 2..n4 {
        if is_comp[i] == 0 {
            chk4 += i as i64;
        }
    }
    let us4 = t4_0.elapsed().as_micros();

    // T5: Parallel Mandelbrot 1024x1024
    let mut row_sums = vec![0i64; w5];
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
    let t5_0 = Instant::now();
    let chunk = (w5 + workers - 1) / workers;
    std::thread::scope(|s| {
        let mut base_y = 0usize;
        for slice in row_sums.chunks_mut(chunk.max(1)) {
            let y0 = base_y;
            base_y += slice.len();
            s.spawn(move || {
                for (off, out) in slice.iter_mut().enumerate() {
                    let py = y0 + off;
                    let cy = -1.25 + (py as f64 * 2.5) / (w5 as f64);
                    let mut rsum: i64 = 0;
                    for px in 0..w5 {
                        let cx = -2.0 + (px as f64 * 3.0) / (w5 as f64);
                        let mut zr = 0.0f64;
                        let mut zi = 0.0f64;
                        let mut it = 0i64;
                        while it < iter5 && (zr * zr + zi * zi <= 4.0) {
                            let tmp = zr * zr - zi * zi + cx;
                            zi = 2.0 * zr * zi + cy;
                            zr = tmp;
                            it += 1;
                        }
                        rsum += it;
                    }
                    *out = rsum;
                }
            });
        }
    });
    let chk5: i64 = row_sums.iter().sum();
    let us5 = t5_0.elapsed().as_micros();

    let total_us = t_all0.elapsed().as_micros();
    println!("T1_US {}\nT1_CHK {:.6}", us1, chk1);
    println!("T2_US {}\nT2_CHK {}", us2, chk2);
    println!("T3_US {}\nT3_CHK {:.6}", us3, chk3);
    println!("T4_US {}\nT4_CHK {}", us4, chk4);
    println!("T5_US {}\nT5_CHK {}", us5, chk5);
    println!("TOTAL_US {}", total_us);
}
