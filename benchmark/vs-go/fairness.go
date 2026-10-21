// The Go side of the scheduler case: N CPU-bound workers, each spinning
// until a wall-clock deadline, counting iterations.
//
//   go run fairness.go [workers] [window_ms]
//
// This is benchmark/actor/fairness.ta, call for call: 32 workers, the same
// 3s window, the same clock check per iteration. The worker that gets more
// CPU does more iterations, so the spread of the counts is the unfairness.
//
// The Go side exists to make the comparison reproducible in-repo: the
// numbers the README quotes used to live in prose only.
package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

func main() {
	workers := 32
	if len(os.Args) > 1 {
		workers, _ = strconv.Atoi(os.Args[1])
	}
	window := 3000
	if len(os.Args) > 2 {
		window, _ = strconv.Atoi(os.Args[2])
	}

	deadline := time.Now().Add(time.Duration(window) * time.Millisecond)
	counts := make([]int64, workers)

	var wg sync.WaitGroup
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			var c int64
			for time.Now().Before(deadline) {
				c++
			}
			counts[i] = c
		}(i)
	}
	wg.Wait()

	mx, mn := counts[0], counts[0]
	for _, c := range counts {
		if c > mx {
			mx = c
		}
		if c < mn {
			mn = c
		}
	}
	dev := (mx - mn) * 100000 / mx
	fmt.Printf("max=%d min=%d dev_x100000=%d\n", mx, mn, dev)
}