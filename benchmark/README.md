# TinyActor Benchmark Suite

## 概述

性能测试框架，用于跟踪 TinyActor 运行时的性能变化并检测性能回归。

## 使用方法

```bash
# 运行所有 benchmark
make benchmark

# 运行特定类别的 benchmark
./benchmark/run_benchmarks.sh core        # 核心性能
./benchmark/run_benchmarks.sh actor       # Actor 并发
./benchmark/run_benchmarks.sh gc          # GC 压力
./benchmark/run_benchmarks.sh compiler    # 编译器性能

# 运行并检测性能回归（超过 10% 变慢会警告）
make benchmark-regression

# 清理 benchmark 结果
make benchmark-clean

# 编译时间基准（benchmark/compile/，median-of-11，issue #258）
make benchmark-compile
make benchmark-compile MODE=ratio   # cold→warm 缓存收益

# TA vs Go 端到端对比（benchmark/vs-go/）：同一压测客户端打 server.ta / server.go
make benchmark-vs-go
```

## 独立套件

有两个套件不在 `run_benchmarks.sh` 的类别列表里（所以 `make benchmark` / CI 不会跑它们），各有独立的 make 目标：

- **`benchmark/compile/`**（`make benchmark-compile`）— 编译时间基准，median-of-11，跟踪冷/热缓存下的编译耗时（issue #258）。
- **`benchmark/vs-go/`**（`make benchmark-vs-go`）— 端到端 HTTP 服务对比：同一个压测客户端分别压 TA 写的服务和 Go 写的服务，比吞吐和延迟分布。需要 go 工具链。

各自目录下的 README 有详细说明。

## Benchmark 类别

### Core (核心性能)
- `fib` - 递归 Fibonacci 计算
- `list-map` - 列表映射和遍历
- `tailcall` - 尾调用递归深度
- `collatz` - Collatz 序列计算（尾递归 + 分支密集整数运算，1M 上限约 12s；issue #119 perf 系列的 5M 版本仅用于手工深度 profiling）
- `ack` - Ackermann A(3,9)，深层非尾递归调用吞吐（移植自 cora `test/benchmark/ack.cora`，约 0.7s）
- `tak` - Takeuchi tak(26,20,10)，每层三路嵌套递归（移植自 cora `test/benchmark/tak.cora`，约 0.7s）
- `takl` - Takeuchi-on-lists mas(24,16,8)，列表递归 + `pair?`/`cdr`（移植自 cora `test/benchmark/takl.cora`，约 1.5s）

### Actor (并发性能)

每条 actor 基准都显式钉 `NWORKERS`，成对记录（mt-revival.md §6 D5 双向守）：基础条目跑单 worker（`NWORKERS=1`），`-mt` 镜像条目跑 `NWORKERS=ncpu`。两侧各自一条 `results.json` 序列，任一侧回退都不会被另一侧的噪声掩盖；VM 默认 worker 数将来怎么改，两侧测的是什么也不会跟着变。

- `message-throughput` / `message-throughput-mt` - 消息传递吞吐量（单 worker / 线程池）
- `spawn` - Actor 创建开销（单 worker）
- `spawn1m` - 1M actor 内存/规模压测（macOS 下经 /usr/bin/time -l 报告峰值 RSS；钉单 worker，池的每 worker 栈会让 RSS 与历史数字不可比）
- `fairness` / `fairness-mt` - 调度公平性（32 个 CPU-bound actor 跑 3s，超订并输出进度偏差；单 worker = 一个 worker 轮转全部 32 个，`-mt` = 摊到 ncpu 个 worker；TA ~0.3-0.5%，同协议 Go ~2.1-2.5%）

### GC (垃圾回收)
- `tree` - 二叉树构建和遍历（Boehm GC benchmark）
- `string-churn` - 字符串分配压力

### Compiler (编译器)
- `tokenizer` - tokenizer.ta 编译时间
- `parser` - parser.ta 编译时间

## 结果存储

Benchmark 结果存储在 `benchmark/results/` 目录下：

- `results.json` - JSON 格式的完整历史记录
- `history.csv` - CSV 格式的历史记录（用于绘图和分析）

结果包含以下信息：
- Timestamp
- Git commit hash
- Category 和 benchmark 名称
- 执行时间（秒）
- 输出和退出码

## 性能趋势分析

### 查看历史趋势

```bash
# 提取特定 benchmark 的历史
jq -r '.[] | select(.name == "fib") | "\(.timestamp), \(.time)"' benchmark/results/results.json

# 绘制趋势图（需要 gnuplot）
jq -r '.[] | select(.name == "fib") | "\(.timestamp) \(.time)"' benchmark/results/results.json | \
  gnuplot -e "plot '-' using 2 with lines title 'fib'; pause -1"
```

### 检测回归

```bash
# 自动检测回归（超过 10% 变慢会警告）
make benchmark-regression
```

## 添加新的 Benchmark

1. 在相应目录下创建 `.ta` 文件（如 `benchmark/core/new-bench.ta`）
2. 在 `benchmark/run_benchmarks.sh` 中添加测试代码：

```bash
result=$(run_benchmark "core/new-bench" \
  "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/core/new-bench.ta")
parse_bench_result "$result"
time=$BM_TIME output=$BM_OUTPUT exit_code=$BM_EXIT

save_result "core" "new-bench" "$time" "$output" "$exit_code"
print_result "new-bench" "$time" "$output" "$exit_code"

if [ $REGRESSION_CHECK -eq 1 ]; then
  check_regression "core" "new-bench" "$time"
fi
```

解析返回值必须用 `parse_bench_result`（`benchmark/lib.sh`）：`output` 可能含多行，`cut -d'|'` 按行解析会把 `exit_code` 切成空串、被误判为失败。

## Benchmark 指导原则

- **可重复性** - Benchmark 应该有确定性的输出
- **短时间** - 每个benchmark 应在几秒内完成
- **有意义的输出** - 打印结果用于验证正确性
- **单一关注点** - 每个benchmark 测试一个特定方面
- **可移植性** - 使用标准库函数，避免 I/O 依赖

## 外部参考

- [R7RS Benchmarks](https://github.com/ecraven/r7rs-benchmarks) - Scheme 语言性能基准
- [Gabriel Benchmarks](https://github.com/ecraven/r7rs-benchmarks) - 经典 Lisp/Scheme benchmark 套件
- [Larceny Benchmarks](http://www.larcenists.org/benchmarksAboutR7.html) - Larceny Scheme 的 benchmark 框架