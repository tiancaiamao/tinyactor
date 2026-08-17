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
```

## Benchmark 类别

### Core (核心性能)
- `fib` - 递归 Fibonacci 计算
- `list-map` - 列表映射和遍历
- `tailcall` - 尾调用递归深度

### Actor (并发性能)
- `message-throughput` - 消息传递吞吐量（⚠️ 受 while 循环内 send 丢消息 bug 影响，当前输出为空，待修复）
- `spawn` - Actor 创建开销
- `spawn1m` - 1M actor 内存/规模压测（macOS 下经 /usr/bin/time -l 报告峰值 RSS）
- `fairness` - 调度公平性（32 个 CPU-bound actor 超订运行在默认核数上 3s，输出进度偏差；TA ~0.3-0.5%，同协议 Go ~2.1-2.5%）

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
run_benchmark "core/new-bench" \
  "cd '$PROJECT_DIR' && '$TINYACTOR' run benchmark/core/new-bench.ta"
time=$(echo "$result" | cut -d'|' -f1)
output=$(echo "$result" | cut -d'|' -f2)
exit_code=$(echo "$result" | cut -d'|' -f3)

save_result "core" "new-bench" "$time" "$output" "$exit_code"
print_result "new-bench" "$time" "$output" "$exit_code"

if [ $REGRESSION_CHECK -eq 1 ]; then
  check_regression "core" "new-bench" "$time"
fi
```

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