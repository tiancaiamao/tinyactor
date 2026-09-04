# kernfuzz 操作手册

内核模糊测试工具链的用户文档。设计原理与裁决规则见
`docs/kernel-fuzzing-design.md`（§ 编号在下文引用）；本文只讲**怎么跑**。

所有 Python 工具仅依赖宿主 Python3 标准库（DEC-6）。

---

## 1. 日常入口（推荐）

### `make kernfuzz-fast` — push 快速环（~3.7 min）

```
make kernfuzz-fast
make kernfuzz-fast KERNFUZZ_FAST_SCALE=1.0   # 全量 500 程序组合（~10 min）
```

三个子环（§9）：

1. **morph 差分**：300 固化 seed 基线 + 200 滚动 seed（M-7，随日期+commit+计数器变化）× Tier A/B 变换 × 4 执行单元
2. **fmt 幂等扫描**（同一批语料）
3. **typecheck 固化负例重放**（`test/kernfuzz-frozen/tc-negative/`，1418 例）

**退出语义**：工具链缺失（如无 Python3）→ 打印 `KERNFUZZ-SKIPPED` + exit 0（不阻塞 CI）；出现任何 finding → exit 1。

### `make kernfuzz-nightly` — 夜间慢速环（全量 1-2 小时）

```
make kernfuzz-nightly                              # 全量
make kernfuzz-nightly KERNFUZZ_NIGHTLY_SCALE=0.05  # 抽验（~几分钟）
```

六个环节（§9）：golden 子集语料全量对拍 → morph 1000 程序（Tier A+B）→ typecheck 双向 oracle 2000 例 → GC 顺序差分 → 消息多重集守恒 → TSan probe，外加 **CPS 环节**（4-way 裁决，gate 全绿后默认开启，`--no-cps` 可关）。

其余变量：`KERNFUZZ_NIGHTLY_MULTIK`、`KERNFUZZ_NIGHTLY_MULTIM`、`KERNFUZZ_NIGHTLY_SEQDIFF_STRESS`（对应 multiset/gc_seqdiff 参数）。

### `make test` — 常规测试

不涉及 kernfuzz；但 kernfuzz 工具自身的单元测试用：

```
cd tools/kernfuzz && for t in test_*.py; do python3 "$t" || break; done
```

---

## 2. 单工具速查

### 程序生成

```bash
python3 tools/kernfuzz/gen.py --seed 42              # 单程序打印到 stdout
python3 tools/kernfuzz/gen.py --seed 0 --count 50 --out-dir /tmp/corpus
```

`--max-depth` 控制表达式深度预算（默认 4）。

### morph 星形对拍（主 oracle，抓 VM bug）

```bash
python3 tools/kernfuzz/morph.py --seeds 10000            # 单 seed
python3 tools/kernfuzz/morph.py --seeds 10000..10005     # 闭区间（注意是两个点）
python3 tools/kernfuzz/morph.py --seeds 10000..10500 --tier A   # 只跑 Tier A 变换
```

finding 落盘到 `--out`（默认 `tools/kernfuzz/build/findings/`，gitignored）。

### 失败归约（拿到 finding 后）

```bash
python3 tools/kernfuzz/reduce.py <finding 目录>            # 读 meta.json 自动取分类
python3 tools/kernfuzz/reduce.py --category crash foo.ta  # 直接给源文件
```

三段归约（行删除 → 子树字面量化 → ddmin），`--budget-s` 控制时间预算。

### typecheck oracle（L2）

```bash
python3 tools/kernfuzz/tc_oracle.py --seeds 10000..10200   # 双向 oracle（5 类变异）
python3 tools/kernfuzz/tc_meta.py  --seeds 10000..10200    # 元性质（确定性/顺序无关/fmt 幂等）
```

### GC / scheduler（L3）

```bash
# 顺序差分：normal vs TA_GC_STRESS=N，输出必须 byte 级一致
python3 tools/kernfuzz/gc_seqdiff.py --seeds 10000:10010 --stress-n 1   # 半开区间 A:B（注意是冒号）
python3 tools/kernfuzz/gc_seqdiff.py --seeds 10000:10200 --stress-n 1,2 --json

# 三类 GC 敌意负载（W-pure / W-msg / W-chaos）
python3 tools/kernfuzz/gc_workloads.py --kind pure  --seed 7
python3 tools/kernfuzz/gc_workloads.py --kind msg   --seed 7
python3 tools/kernfuzz/gc_workloads.py --kind chaos --seed 7 --scale 2

# 消息多重集守恒（W-msg）
python3 tools/kernfuzz/multiset.py --seeds 0:100 --k 4 --m 25
```

### CPS 变换器（Tier C）

```bash
python3 tools/kernfuzz/cps.py --emit test/basic/arith.ta      # 打印 CPS 化后的源码（参数是 .ta 源文件）
python3 tools/kernfuzz/cps.py --file test/basic/arith.ta       # 单文件 4-way 恒等校验
python3 tools/kernfuzz/cps.py --corpus 200 --start 10000               # 生成语料窗口全量 gate
python3 tools/kernfuzz/cps.py --tco                                   # TCO 深度自检
```

四种模式互斥（`--emit` / `--file` / `--corpus N` / `--tco`）。

4-way 裁决：`run(orig)` / `golden(orig)` / `run(cps)` / `golden(cps)` 两两比对，
自动区分 transformer-mismatch / suspect-vm / anchor-diverge（§5.2）。

### VM 层 GC 压力旋钮（唯一内核改动）

```bash
TA_GC_STRESS=1 ./tinyactor run foo.ta    # 每次分配都强制当前进程 GC（最狠档）
TA_GC_STRESS=8 ./tinyactor run foo.ta
```

不设置时行为与改动前完全一致（默认零行为变化，DEC-5）。

---

## 3. finding 的生命周期

```
morph / tc_oracle / gc_seqdiff / multiset 发现差异
  → 各自落盘（morph: tools/kernfuzz/build/findings/；其余见命令输出末尾的 findings dir 行，均 gitignored；
    含源码、4 单元输出、meta.json）
  → 签名与 test/kernfuzz-frozen/morph-known-signatures.txt 比对：已知 → 回绿跳过
  → 未知 → exit 1 → reduce.py 归约 → 确认是 VM/编译器 bug → 记录待独立 PR（不在主线修）
```

`test/kernfuzz-frozen/` 下的 1477 个数据文件（快照 .sexp / tc 负例 / seed 清单）都是
**机器生成的固化基线**，保证跨 clone、跨时间可复现，不需要人工维护。

## 4. 滚动 seed 复现

fast/nightly 的滚动 seed 由「日期 + commit + 计数器」哈希派生（M-7），同一输入必然同一序列：

```bash
python3 tools/kernfuzz/fast.py    rolling-seeds --date 2026-09-04 --counter 0
python3 tools/kernfuzz/nightly.py rolling-seeds --date 2026-09-04 --counter 0
# 可选 --count N / --git-sha SHA；计数器状态存于 gitignored 独立文件，fast 与 nightly 各自独立递增
```