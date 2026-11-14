package main

import (
	"fmt"
	"runtime"
	"sync"
	"time"
)

type Node struct {
	x int64
	y int64
	z int64
}

func fib(n int64) int64 {
	if n <= 1 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func stepFn(a, b int64) int64 {
	next := a + b
	if next >= 1000003 {
		next -= 1000003
	}
	return next
}

func main() {
	tAll := time.Now()

	// Task 1
	const reps1 int64 = 100
	const n1 int64 = 1000000
	var step1 float32 = 0.000001
	var totalPi1 float32 = 0.0
	t1 := time.Now()
	for r := int64(0); r < reps1; r++ {
		var sum1 float32 = 0.0
		var x1 float32 = 0.0000005
		for i := int64(0); i < n1; i++ {
			sum1 = sum1 + 4.0/(1.0+x1*x1)
			x1 = x1 + step1
		}
		totalPi1 += sum1 * step1
	}
	us1 := time.Since(t1).Microseconds()
	pi1 := totalPi1 * 0.01

	// Task 2
	const n2 int = 50000000
	t2 := time.Now()
	arr2 := make([]int64, n2)
	var cur2 int64 = 1
	for i := 0; i < n2; i++ {
		cur2 += 7
		if cur2 >= 1024 {
			cur2 -= 1024
		}
		arr2[i] = cur2
	}
	var sum2 int64 = 0
	for i := 0; i < n2; i++ {
		v := arr2[i]*3 + 1
		arr2[i] = v
		sum2 += v
	}
	arr2 = nil
	us2 := time.Since(t2).Microseconds()

	// Task 3
	const n3 int = 1000000
	t3 := time.Now()
	nodes3 := make([]*Node, n3)
	for i := 0; i < n3; i++ {
		iv := int64(i)
		nodes3[i] = &Node{x: iv, y: iv * 2, z: iv + 5}
	}
	var sum3 int64 = 0
	for i := 0; i < n3; i++ {
		p := nodes3[i]
		sum3 += p.x + p.y + p.z
	}
	nodes3 = nil
	us3 := time.Since(t3).Microseconds()

	// Task 4
	t4 := time.Now()
	f40 := fib(40)
	var acc4 int64 = 0
	for i := int64(0); i < 100000000; i++ {
		acc4 = stepFn(acc4, 7919)
	}
	us4 := time.Since(t4).Microseconds()
	chk4 := f40 + acc4

	// Task 5
	const rows5 = 2000
	const cols5 = 50000
	t5 := time.Now()
	rowSums := make([]int64, rows5)
	workers := runtime.NumCPU()
	chunk := rows5 / workers
	rem := rows5 % workers
	var wg sync.WaitGroup
	cur := 0
	for w := 0; w < workers; w++ {
		sz := chunk
		if w < rem {
			sz++
		}
		start := cur
		end := cur + sz
		cur = end
		wg.Add(1)
		go func(s, e int) {
			defer wg.Done()
			for r := s; r < e; r++ {
				acc := int64(r)
				for c := 0; c < cols5; c++ {
					acc += 7919
					if acc >= 1000003 {
						acc -= 1000003
					}
				}
				rowSums[r] = acc
			}
		}(start, end)
	}
	wg.Wait()
	var sum5 int64 = 0
	for r := 0; r < rows5; r++ {
		sum5 += rowSums[r]
	}
	us5 := time.Since(t5).Microseconds()

	usTotal := time.Since(tAll).Microseconds()

	fmt.Printf("TASK1_US %d\nTASK1_PI %.6f\nTASK2_US %d\nTASK2_CHK %d\n", us1, pi1, us2, sum2)
	fmt.Printf("TASK3_US %d\nTASK3_CHK %d\nTASK4_US %d\nTASK4_CHK %d\n", us3, sum3, us4, chk4)
	fmt.Printf("TASK5_US %d\nTASK5_CHK %d\nTOTAL_US %d\n", us5, sum5, usTotal)
}
