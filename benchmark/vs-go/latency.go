// The Go side of the sched case: how late does a sleeping goroutine wake up
// while CPU-bound goroutines fill every P?
//
//   go run latency.go [noise_goroutines] [window_ms]
//
// Protocol (identical in latency.ta):
//   * noise_goroutines goroutines spin on the clock until the window ends
//   * one probe goroutine loops: stamp -> sleep(1ms) -> stamp, and keeps
//     every delay
//   * the delays are sorted and the percentiles are printed
//
// The requested sleep is 1ms; what the numbers show is 1000us plus the
// scheduler's wake-up delay — the part this case is about.
package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"
)

func main() {
	noise := 8
	window := 3000
	if len(os.Args) > 1 {
		noise, _ = strconv.Atoi(os.Args[1])
	}
	if len(os.Args) > 2 {
		window, _ = strconv.Atoi(os.Args[2])
	}

	deadline := time.Now().Add(time.Duration(window) * time.Millisecond)

	var wg sync.WaitGroup
	for i := 0; i < noise; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for time.Now().Before(deadline) {
			}
		}()
	}

	var samples []int64
	for time.Now().Before(deadline) {
		t0 := time.Now()
		time.Sleep(time.Millisecond)
		t1 := time.Now()
		samples = append(samples, t1.Sub(t0).Microseconds())
	}
	wg.Wait()

	sort.Slice(samples, func(i, j int) bool { return samples[i] < samples[j] })
	n := len(samples)
	fmt.Printf("samples=%d\n", n)
	fmt.Printf("p50_us=%d\n", samples[n/2])
	fmt.Printf("p90_us=%d\n", samples[(n*9)/10])
	fmt.Printf("p99_us=%d\n", samples[(n*99)/100])
	fmt.Printf("max_us=%d\n", samples[n-1])
}