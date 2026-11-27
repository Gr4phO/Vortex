package main

import (
	"fmt"
	"math"
	"runtime"
	"sync"
	"time"
)

func qsortI64(arr []int64, lo, hi int) {
	i, j := lo, hi
	pivot := arr[(lo+hi)/2]
	for i <= j {
		for arr[i] < pivot {
			i++
		}
		for arr[j] > pivot {
			j--
		}
		if i <= j {
			arr[i], arr[j] = arr[j], arr[i]
			i++
			j--
		}
	}
	if lo < j {
		qsortI64(arr, lo, j)
	}
	if i < hi {
		qsortI64(arr, i, hi)
	}
}

func main() {
	n1, steps1 := 600, 25
	n2 := 500000
	n3 := 320
	n4 := 10000000
	w5, iter5 := 1024, 256

	tAll0 := time.Now()

	// T1
	x := make([]float64, n1)
	y := make([]float64, n1)
	z := make([]float64, n1)
	vx := make([]float64, n1)
	vy := make([]float64, n1)
	vz := make([]float64, n1)
	m := make([]float64, n1)
	for i := 0; i < n1; i++ {
		x[i] = float64((i*17)%100)*0.1 - 5.0
		y[i] = float64((i*31)%100)*0.1 - 5.0
		z[i] = float64((i*47)%100)*0.1 - 5.0
		vx[i] = float64((i%7)-3) * 0.01
		vy[i] = float64(((i*3)%7)-3) * 0.01
		vz[i] = float64(((i*5)%7)-3) * 0.01
		m[i] = 0.5 + float64(i%5)*0.25
	}
	dt, soft := 0.01, 0.01
	t1_0 := time.Now()
	for s := 0; s < steps1; s++ {
		for i := 0; i < n1; i++ {
			fx, fy, fz := 0.0, 0.0, 0.0
			xi, yi, ziv := x[i], y[i], z[i]
			for j := 0; j < n1; j++ {
				if j != i {
					dx := x[j] - xi
					dy := y[j] - yi
					dz := z[j] - ziv
					d2 := dx*dx + dy*dy + dz*dz + soft
					inv := 1.0 / math.Sqrt(d2)
					f := m[j] * inv * inv * inv
					fx += dx * f
					fy += dy * f
					fz += dz * f
				}
			}
			vx[i] += fx * dt
			vy[i] += fy * dt
			vz[i] += fz * dt
		}
		for i := 0; i < n1; i++ {
			x[i] += vx[i] * dt
			y[i] += vy[i] * dt
			z[i] += vz[i] * dt
		}
	}
	chk1 := 0.0
	for i := 0; i < n1; i++ {
		chk1 += 0.5 * m[i] * (vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i])
	}
	us1 := time.Since(t1_0).Microseconds()

	// T2
	arr2 := make([]int64, n2)
	var lcg int64 = 12345
	for i := 0; i < n2; i++ {
		lcg = (lcg * 48271) % 2147483647
		arr2[i] = lcg
	}
	t2_0 := time.Now()
	qsortI64(arr2, 0, n2-1)
	chk2 := arr2[0] + arr2[250000] + arr2[n2-1]
	us2 := time.Since(t2_0).Microseconds()

	// T3
	a3 := make([]float64, n3*n3)
	b3 := make([]float64, n3*n3)
	c3 := make([]float64, n3*n3)
	for i := 0; i < n3; i++ {
		for k := 0; k < n3; k++ {
			a3[i*n3+k] = float64((i*7+k*13)%97) * 0.01
			b3[i*n3+k] = float64((i*11+k*5)%89) * 0.01
		}
	}
	t3_0 := time.Now()
	for i := 0; i < n3; i++ {
		crow := c3[i*n3 : (i+1)*n3]
		for k := 0; k < n3; k++ {
			aik := a3[i*n3+k]
			brow := b3[k*n3 : (k+1)*n3]
			for j := 0; j < n3; j++ {
				crow[j] += aik * brow[j]
			}
		}
	}
	chk3 := 0.0
	for _, v := range c3 {
		chk3 += v
	}
	us3 := time.Since(t3_0).Microseconds()

	// T4
	isComp := make([]int32, n4)
	t4_0 := time.Now()
	for p := 2; p*p < n4; p++ {
		if isComp[p] == 0 {
			for m4 := p * p; m4 < n4; m4 += p {
				isComp[m4] = 1
			}
		}
	}
	var chk4 int64
	for i := 2; i < n4; i++ {
		if isComp[i] == 0 {
			chk4 += int64(i)
		}
	}
	us4 := time.Since(t4_0).Microseconds()

	// T5
	rowSums := make([]int64, w5)
	workers := runtime.NumCPU()
	t5_0 := time.Now()
	var wg sync.WaitGroup
	chunk, rem, cur := w5/workers, w5%workers, 0
	for w := 0; w < workers; w++ {
		sz := chunk
		if w < rem {
			sz++
		}
		y0, y1 := cur, cur+sz
		cur += sz
		wg.Add(1)
		go func(startY, endY int) {
			defer wg.Done()
			for py := startY; py < endY; py++ {
				cy := -1.25 + (float64(py)*2.5)/float64(w5)
				var rsum int64
				for px := 0; px < w5; px++ {
					cx := -2.0 + (float64(px)*3.0)/float64(w5)
					zr, zi, it := 0.0, 0.0, 0
					for it < iter5 && (zr*zr+zi*zi <= 4.0) {
						tmp := zr*zr - zi*zi + cx
						zi = 2.0*zr*zi + cy
						zr = tmp
						it++
					}
					rsum += int64(it)
				}
				rowSums[py] = rsum
			}
		}(y0, y1)
	}
	wg.Wait()
	var chk5 int64
	for _, v := range rowSums {
		chk5 += v
	}
	us5 := time.Since(t5_0).Microseconds()

	totalUs := time.Since(tAll0).Microseconds()
	fmt.Printf("T1_US %d\nT1_CHK %.6f\n", us1, chk1)
	fmt.Printf("T2_US %d\nT2_CHK %d\n", us2, chk2)
	fmt.Printf("T3_US %d\nT3_CHK %.6f\n", us3, chk3)
	fmt.Printf("T4_US %d\nT4_CHK %d\n", us4, chk4)
	fmt.Printf("T5_US %d\nT5_CHK %d\n", us5, chk5)
	fmt.Printf("TOTAL_US %d\n", totalUs)
}
