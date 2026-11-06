# kernfuzz — 操作手册与设计理由

`tools/kernfuzz/` 是内核（tokenizer / parser / typecheck / codegen / VM）的自动化压力场。
**整个工具链只有一门语言：TA**。宿主解释器、Scheme 参考实现、pytest 套件在 P8 收口时
全部删除（§11），所以跑这个环需要的东西正好是仓库自己构建的东西——`tinyactor`、`tavm`、
`vendor/`——没有第三方运行时。

- **要动手跑**：§5（环与参数）、§6（加一条冻结语料）、§7（读一个 finding）。
- **要理解为什么这么设计**：§2–§3。
- **想改这个环**：先读 §1.1 的判据和 §3 的 DEC 条目；改 `tools/kernfuzz/*.ta` 之后
  `make kernfuzz-fast` 与 `make kernfuzz-snapshot-check` 必须绿。

---

## 1. 这个环在测什么

### 1.1 唯一被测契约：同一份源码，两条编译路径

```
P.ta ──(tinyactor build)──> a.tabc ──(tavm)──> 输出 A     ← A 臂（anchor）
P.ta ──(tavm lib/bootstrap.tabc P.ta b.tabc)──> b.tabc ──(tavm)──> 输出 B   ← B 臂（backend）
```

A 臂走 `tinyactor build`（自举锚编译器的常规入口），B 臂走入库种子
`lib/bootstrap.tabc` 里的 self-hosted driver。**同一份源码经两套互不相同的 front end
到达同一个 VM**，于是"两条臂的输出不同"就是"其中一条 front end 错了"的可执行证据。
这个契约不需要参考实现，也不需要知道正确答案是什么。

两个前提：

- 两条臂共享 VM 与 GC。**它们共有的 bug 在差分里是隐形的**——这是 DEC-4 让两条 run 半程
  再跑一遍 `tavm_asan` 的原因（内存错误由 sanitizer 看见，不由差分看见）。
- 判据是**输出相等**，不是"输出正确"。所以这个环能证明"两条路径不一致"，不能证明"程序
  语义符合 spec"；后者归 `test/basic/`、`test/compiler/` 与 `docs/ta-language-spec.md`。

### 1.2 三个子环，一次运行

`tools/kernfuzz/fast.ta` 一个进程跑完三件事，各自印一行，退出码由一处统一给出：

| 环 | 断言 | 红叫什么 |
|----|------|---------|
| 1/3 差分 | 两条臂的 run 半程输出相等（§3 DEC-1/DEC-3） | `mismatch` / `build-fail` / `hang` / `tavm-crash` … |
| 2/3 fmt 幂等 | 格式化不改变程序 | `fmt-drift`（第二次 fmt 不是不动点）/ `fmt-semantics`（格式化后成了另一个程序） |
| 3/3 tc 冻结负例重放 | 每条已采集负例的结论不漂移 | 结论从 `reject` 变成 `accept-*`（或反之） |

---

## 2. 为什么做这件事

1. 内核是所有上层正确性的地基，组合空间是表达式语法树的笛卡尔积，手写测试只能覆盖想到的边界。
   历史上整批 parser hang 回归（`test/compiler/fuzz-regression.ta`）证明自动化搜索有效。
2. 这个仓库有两个 front end（自举 driver 与常规编译入口），**它们必须互相一致**——
   一致性只能被比对出来，不能被单侧测试证明。这是本环最便宜也最致命的断言。
3. bug 不是被找到的，是被逼出来的：环的价值在于让不一致无处藏身，而不是在于它跑得多快。

---

## 3. 关键设计决策

### DEC-1：oracle = 同源双路径，不是参考实现

曾经有过"第三条腿"：一个独立的参考解释器（Scheme 实现，后来 Python 重写），
拿它和 VM 比。放弃它的理由不是贵，而是**不独立**：任何外部解释器都要么复用本仓库的
parser（与 VM 共享 bug），要么自带一套语义（那测的是"两套语义谁对"，与内核回归无关）。
双路径差分只假设一件几乎必然的事——两套 front end 不会以同样的方式错——换来的是一条
不需要维护任何期望值的判据。见 §11 的删除账。

### DEC-2：一门语言（TA）

编排、生成、比对、记录、归约、快照全部是 `tools/kernfuzz/*.ta`，用 `./tinyactor run` 跑。
理由：环的工具代码本身是对 TA 的日常使用（缺什么能力，写环的时候就会疼），
而一个需要额外装解释器的环，在 CI 里失修的频率高于它抓 bug 的频率。
P8 §1 定的终点是"同一程序两条编译路径比对"这条价值，其余全是实现选择。

### DEC-3：判据只有一处

`tafuzz_cmp.verdict` 是唯一的判定入口：环用它，归约器用它，自检用它。

- 比什么：run 半程的 stdout、exit code、stderr（崩溃文本）。**不比 build 半程的 stdout**
  （B 臂 driver 在那里印自己的 `nil`）与它的 stderr（各 front end 各自报进度）。
- 归一化刻意窄：只去行尾空白与尾部空行；崩溃文本里只把 `pid <n>` 掩成 `pid N`（actor pid
  是分配顺序的产物）。其余逐字节比。
- 判定顺序固定（先命中先算），所以一个 finding 的 category 可复现：
  hang > build 失败 > sanitizer abort > exit code > stdout > 崩溃文本。

一个判据而不是三个，是"红在哪里"能被信任的前提：归约器复用同一个判据，缩到最小还能
红，说明缩掉的确实无关。

### DEC-4：ASan 是第三条腿

两条臂都在 `tavm_asan` 存在时跑插桩版。理由只有一句：**差分看不见两条臂共享的 bug**，
而内存错误正是 VM 里最典型的一种。sanitizer 缺席不静默降级——summary 里写
`KERNFUZZ-DEGRADED`，跑的是裸 VM，读者知道这一轮的结论少一条腿。

### DEC-5：冻结语料只读

`test/kernfuzz-frozen/` 是**数据**，是这一套断言的参照系。环只读它；唯一的写者是
`tools/kernfuzz/snapshot.ta`，而它写完必须满足 `git status -- test/kernfuzz-frozen/`
为空（§6.3）。参照系被环自己改写的话，"绿"就退化成"环同意环"。

### DEC-6：签名去重（M-10）

finding 的签名是 `<category>:<sha256(去空白源码)[:16]>`，与 P8 之前的环逐字节同格式。
`test/kernfuzz-frozen/morph-known-signatures.txt` 里已 triage 的签名会在启动时预注入：
命中即"这个程序这条差异已经知道"，环保持绿而不是每天报同一次警。新签名 → exit 1。

---

## 4. 目录与角色

| 路径 | 角色 |
|------|------|
| `tools/kernfuzz/fast.ta` | 环本体：三个子环 + 滚动 seed 派生 + 规模换算 + 退出码 |
| `tools/kernfuzz/tafuzz.ta` | 单程序入口：生成/选 seed、跑两臂、判定、记录；带 `--self-check` |
| `tools/kernfuzz/tafuzz_gen.ta` | 确定性生成器（seed → 程序文本，§5.4） |
| `tools/kernfuzz/tafuzz_arm.ta` | 两条臂与四相采集（compile/run 各半程），每相一个硬预算 |
| `tools/kernfuzz/tafuzz_cmp.ta` | 唯一判据：`verdict` / `category` / 归一化（`norm` / `crash_key`） |
| `tools/kernfuzz/tafuzz_finding.ta` | 签名、已知签名去重、finding 目录落盘（§7） |
| `tools/kernfuzz/tafuzz_check.ta` | 自检用例集（判定、掩码、hang、归约器的行为） |
| `tools/kernfuzz/tafuzz_reduce.ta` | 归约器（§5.5） |
| `tools/kernfuzz/snapshot.ta` | 语料 AST 快照生成器 + 孤儿清扫（§6.3） |
| `tools/kernfuzz/ast-dump.ta` | 单个 `.ta` 文件的 AST dump（s-expr 编码的**唯一**权威） |
| `tools/kernfuzz/build/` | 全部 scratch（gitignored）：workspace、cache、findings、selfcheck |
| `test/kernfuzz-frozen/` | 冻结数据，只读（§6） |

`tools/kernfuzz/lsan.supp` 是净化器抑制清单：**今天没有调用方，但它不是死文件**。
它圈的是 `ta_resource_new` 那处 by-design 的进程期分配（`docs/c-resource-design.md` §7
裁决 ③）——Linux 的 LSan 会在退出时把它报成 leak 并按 `exitcode=42` 算 backend 崩溃，
于是 B 臂编译每次必红；macOS 的 ASan 没有 LSan，本地复现不了。接线不白给：把
`suppressions=` 塞进 `ASAN_OPTIONS`（`tafuzz_arm.asan_env`）会让 **macOS 直接 abort**
（`AddressSanitizer: failed to parse suppressions`，实测 rc=134，因为没有 LSan 的运行时
不认识 `leak:` 这种类型），所以要么加平台分支，要么留作 Linux 专用数据。取后者：
文件留着，删除 = 把一条只在 Linux 上出现的假红放回来。见 §10。

---

## 5. 操作

### 5.0 跑之前的三个事实

1. **从仓库根跑**：`tinyactor run` 以 cwd 解析 `import`（`vendor/tinyactor`），
   在别处跑会 import 不到库。
2. **所有 scratch 落在 `build/` 与 `tools/kernfuzz/build/`**，两者 gitignored；一轮跑完
   工作区应当只有 `test/kernfuzz-frozen/` 之外的零改动。
3. **依赖**：`tinyactor`、`tavm`、`lib/bootstrap.tabc`、`vendor/`（B 臂要用种子 driver）。
   缺任何一个 → `KERNFUZZ-SKIPPED <reason>` 且 exit 0（finding 才算红）。

### 5.1 跑一次环

```sh
make kernfuzz-fast        # CI 每 push 跑的就是这条（默认 scale 0.4）
make kernfuzz-nightly     # 同一套环，scale 1.0 + 更长的滚动尾巴（每晚 03:00 UTC）
```

手工跑（同一入口，自己挑规模）：

```sh
./tinyactor run tools/kernfuzz/fast.ta                                  # 默认 0.4
./tinyactor run tools/kernfuzz/fast.ta --scale 0.02                     # 几十秒的小跑
./tinyactor run tools/kernfuzz/fast.ta --scale 1.0 --count 0            # 全量冻结集，不派新种子
./tinyactor run tools/kernfuzz/fast.ta rolling-seeds --date 2026-09-04 \
    --counter 0 --count 5 --git-sha abc123                             # 只印 M-7 派生的 seed
```

单程序（调试一个 seed 时用它，别用整环）：

```sh
./tinyactor run tools/kernfuzz/tafuzz.ta --dump 10007          # 只印生成出来的程序
./tinyactor run tools/kernfuzz/tafuzz.ta --limit 20            # 有界跑一批默认 seed
./tinyactor run tools/kernfuzz/tafuzz.ta --seeds 10005,10007 --budget 5000
```

### 5.2 参数与规模

| 参数 | 作用 |
|------|------|
| `--scale <f>` | 三个子环按同一比例缩放：固定 seed、滚动 seed、tc 用例各取 `round(total*f)` |
| `--count <n>` | **只**覆盖滚动 seed 条数（`--scale` 表达不了"更多"，因为 1.0 是上限） |
| `KERNFUZZ_FAST_SCALE` | fast 目标喂给 `--scale` 的值（默认 0.4），CI 已知的旋钮保留原名 |
| `KERNFUZZ_NIGHTLY_COUNT` | nightly 目标喂给 `--count` 的值（默认 700） |
| `--budget <ms>` | 单个程序 run 半程的预算（默认 2000；compile 半程按 `compile_factor` 放宽） |

规模对照（`--scale 1.0` 是天花板）：

| 组成 | 1.0（nightly 形状） | 0.4（fast 默认） |
|------|--------------------|------------------|
| 固定 seed | 300 | 120 |
| 滚动 seed | 200（`--count 700` 时 700） | 80 |
| tc 冻结负例 | 1418（全部） | 567 |

**不随机**：同一份 manifest、同一个计数器、同一组数字挑出同一个子集，所以小规模的红在
1.0 上必然复现。

两个参数的分工容易记混，写清楚：`--scale` 决定**固定 seed 条数**与 **tc 用例条数**
（各取 `round(total*scale)`），滚动 seed 在没给 `--count` 时也按它算（`round(200*scale)`）；
`--count` 一旦给出就是**绝对条数**，直接替换滚动 seed 那一路，与 `--scale` 无关。
所以 nightly 的"更大"来自 `--count 700`（滚动尾巴），而它的固定集与 1418 条 tc 仍由
`--scale 1.0` 决定——`--count` 不会让 tc 超过 1418。

退出码（CI 依赖，不要改）：全绿 `0`；出现新 finding `1`；工具链缺失 →
`KERNFUZZ-SKIPPED <reason>` + `0`。

### 5.3 AST dump 与快照

`tools/kernfuzz/ast-dump.ta` 把单个 `.ta` 文件的完整 AST 印成一行 s-expr，
`test/kernfuzz-frozen/ast-nodes.txt` 冻结节点形态表，`snapshot.ta` 把两者合起来变成
"语料 106 个合法文件的 AST 不许漂移"这一条断言。dump 的编码由 `ast-dump.ta` 一处决定，
别在别处再实现一遍渲染。

```sh
./tinyactor run tools/kernfuzz/ast-dump.ta test/basic/closure.ta   # 看一个文件的 AST
```

### 5.4 生成器的确定性（PRNG 约定）

`tafuzz_gen.ta` 没有模块状态：**seed 是输入，程序是输出**，同一个 seed 永远生成同一份
字节。状态是一个普通计数器，每次 draw 显式传下去：

```
s' = sha256("<seed>:<counter>") 的前 8 字节（小端）→ 折进 int48 正数域
```

可复现（finding 里记全 seed 就能重放）、可追溯（程序文本落盘在 finding 目录里）、
可并行（没有共享可变状态）。生成器只产 `typecheck` 能过的程序，所以差分环的红默认是
"两臂不一致"，不是"生成器写错了"。

### 5.5 归约器

```sh
make kernfuzz-minimize FINDING=build/kernfuzz/fast/morph/mismatch:1a2b3c4d5e6f7a8b
make kernfuzz-minimize FINDING=... BUDGET=200      # hang 类 finding：沿用原来的预算
```

（只接受环 1 的目录形态——它必须有 `prog.ta`；环 2/3 的单文件报告不含可复现的差分，§7.1。）

判据是环自己的判据：候选程序经 `tafuzz_arm.sample` + `tafuzz_cmp.verdict` 后
**detail 与原始 finding 相同**即算复现（detail 已经点名了差异通道：`stdout` / `exit` /
`ar_hang` / `crash`）。两个粒度：整行 ddmin 阶梯（半数、四分之一……一行），
以及整块平衡括号子树（从最宽的开始）。token 级第三趟实测过并删掉了——行与子树去掉之后
剩下的只是空白，多一个搜索轴换不来更小的程序。

`make kernfuzz-minimize` 的 prerequisite 里有 `lib/bootstrap.tabc`（B 臂编译半程要用
入库种子），种子过期时这一条会先跑一遍自举——首跑多出约 50 s，产物 byte-identical，
工作区不变；直接 `./tinyactor run tools/kernfuzz/tafuzz_reduce.ta` 跳过 make 就没有这段。

预算按**候选数**（`--evals`，默认 300）而不是秒数：同一 finding 在空闲机器与负载机器上
缩成同样的字节。预算用尽返回已验证的最好结果，不报失败。原程序已经不再产生那个 detail
时**拒绝**并退出非零，绝不"缩成空程序算成功"。

### 5.6 自检

```sh
./tinyactor run tools/kernfuzz/tafuzz.ta --self-check
```

21 个用例，每个都自带期望结局：判定通道（stdout/exit/crash/build-fail）、归一化
（行尾空白、尾部空行、pid 掩码）、hang 与预算、finding 落盘、ASan 臂、以及归约器的
七个行为（含"已不再复现时拒绝"与"重复跑结果一致"）。**自检红了就别信环的绿**：
一个永远绿灯的环比没有环更糟。

---

## 6. 冻结语料：只读，以及怎么加一条

### 6.1 组成

| 文件 | 内容 | 谁写它 |
|------|------|--------|
| `fixed-seeds.txt` | 300 个固定 seed（基准集；程序由生成器确定性再生成） | 手工 |
| `ast-nodes.txt` | 冻结的 AST 节点形态表（权威 = `test/compiler/parser-ast.ta`） | 手工 |
| `test/kernfuzz-frozen/snapshots/*.sexp` | 106 个合法语料文件（`test/basic` 91 + `test/compiler` 15）的 AST dump | `snapshot.ta` |
| `expected-fail.txt` | 应当 dump 失败的语料（文件 → 退出码 → 首行 stderr）；当前为空 | `snapshot.ta` |
| `test/kernfuzz-frozen/tc-negative/manifest.json` + `*.ta` | 1418 条已采集负例及其期望结论 | 手工 + `tafuzz_gen` 的历史产出 |
| `morph-known-signatures.txt` | 已 triage 的 finding 签名（DEC-6） | 手工 |

### 6.2 手工加一条 tc 负例（原 `make kernfuzz-freeze-tc`）

冻结语料的价值在"已采集的 1418 条负例"，不在"用一个生成器批量再产一遍"，所以 Python 时代的
`freeze-tc` 目标**没有移植**，改成两步手工动作：

1. 把程序放进 `test/kernfuzz-frozen/tc-negative/<名字>.ta`；
2. 在 `manifest.json` 的 `entries` 数组末尾（`"frozen-from"` 之前那一行 `],` 前）加一条：

```json
  {
   "expect": "reject",
   "file": "<名字>.ta",
   "note": "手工冻结：为什么这条值得留"
  },
```

**重放环只读 `file` 与 `expect`** 两个字段（`class` / `seed` / `sub` / `why` 是溯源信息，
可省）。`expect` 的取值：`reject`（必须被拒）、`accept-hole`（接受但留洞）、
`accept-quiet`（静默接受）。

复核方式：`--scale 1.0` 才覆盖到新条目（采样是等距取 `entries[i*total/n]`，末尾的条目
只有 n == total 时才会被选中）：

```sh
./tinyactor run tools/kernfuzz/fast.ta --scale 1.0 --count 0   # ring 3/3 cases=1419/1419 ok=1419
```

想**先试后提**就在一次性 worktree 里试：重放环读的是固定目录（`fast.ta` 的 `tc_dir` 是常量，
没有 `--corpus` 之类的开关），所以"指向别处"这条捷径不存在，而 `test/kernfuzz-frozen/`
不许为了试跑改动：

```sh
git worktree add --detach /tmp/wt8e HEAD      # 副本里加条目、跑完即弃
cp tinyactor tavm tavm_asan lib/*.dylib /tmp/wt8e/ 2>/dev/null   # 只拷二进制与模块
cd /tmp/wt8e && ./tinyactor run tools/kernfuzz/fast.ta --scale 1.0 --count 0
git worktree remove --force /tmp/wt8e
```

（实测过这条流程：加一条 `expect: reject` 的手写负例 → `cases=1419/1419 ok=1419`，exit 0；
把该条的 `expect` 改成 `accept-quiet` 再跑 → 环报 `tc-drift` 且 exit 1。判据确实会红。）


### 6.3 重新冻结 AST 快照

语料或 parser 变了之后，`test/kernfuzz-frozen/snapshots/` 要跟着刷新：

```sh
./tinyactor run tools/kernfuzz/snapshot.ta     # 重写快照 + expected-fail.txt + 清扫孤儿
make kernfuzz-snapshot-check                   # 断言 = 重跑后 git status 为空
git add test/kernfuzz-frozen/ && git commit -m "test: refresh frozen snapshots"
```

门（`make kernfuzz-snapshot-check`）的断言就是 `git status --porcelain -- test/kernfuzz-frozen/`
为空——"parser 没漂移"和"冻结树就是 HEAD 说的那样"是同一句话。

生成器**先写后扫**：先把本轮每个文件的快照写掉，收尾只删"本轮没有产出"的名字（语料文件
被删除或改名后的孤儿）。这样门在任何时刻被打断都只留下新增/修改，不会留下一串删除——
一个被 Ctrl-C 打断的断言门不该给出"冻结语料已被删除"的工作区。dump 失败的文件保留旧快照
是有意的：它已被列进 `expected-fail.txt`，那条红是语料的问题，不是清扫的问题。

---

## 7. 读一个 finding

### 7.1 落在哪里

环有两种 finding 形态，取决于那个环能不能"缩"：

```
build/kernfuzz/fast/morph/<category>:<sha256(去空白源码)[:16]>/     ← 环 1
  prog.ta             程序源码，逐字节
  a.compile.{stdout,stderr}   b.compile.{stdout,stderr}
  a.run.{stdout,stderr}       b.run.{stdout,stderr}
  meta.json           signature / category / detail / seed / invocation / 每相 {tag,cmd,exit,ms}

build/kernfuzz/fast/{fmt,tc-replay}/<category>:<16 hex>.txt          ← 环 2、环 3
  category / what / seed / exit / command + --- stdout --- / --- stderr ---
```

单程序入口（`tafuzz.ta`）的 finding 落在 `tools/kernfuzz/build/findings/<sig>/`，形态与环 1 相同。

环 2/3 用单文件而不是目录，是因为它们**不是可归约的差分**：fmt 漂移与 tc 结论漂移说的都是
"这一次构建的结论"，没有两条臂的 detail 可以复现，`make kernfuzz-minimize` 的判据对它们
不成立。能缩的只有环 1 的目录形态。

读的顺序：`meta.json` 的 `category` + `detail` → `a.run.stdout` 与 `b.run.stdout` 的
diff → 若要复现，`prog.ta` 拿去单跑（`./tinyactor run tools/kernfuzz/tafuzz.ta --dump <seed>`），
`make kernfuzz-minimize` 拿去缩。


### 7.2 category 词表（冻结）

| category | 含义 |
|----------|------|
| `mismatch` | 两条臂的 run 半程输出/exit/崩溃文本不同（最常见的那个） |
| `build-fail` | A 臂编译失败（程序本身有问题，或 `tinyactor build` 回归） |
| `backend-build-fail` | B 臂编译失败而 A 臂通过 → 自举 driver 与常规入口不一致 |
| `hang` / `backend-hang` | 某一相的 run/compile 吃满预算被杀 |
| `tavm-crash` | ASan 中止（DEC-4 的第三条腿看见了内存错误） |
| `backend-crash` | B 臂编译半程崩溃 |
| `fmt-drift` / `fmt-semantics` | 环 2 的两类：格式化不是不动点 / 格式化后程序变了 |

### 7.3 已 triage 的签名

一条 finding 判完（确认是 VM bug、或确认是生成器的问题）之后：
- 确认是**已知**差异 → 把签名加进 `test/kernfuzz-frozen/morph-known-signatures.txt`（一行一条）；
- 确认是**新 bug** → 修 bug，然后把回归钉进 `test/basic/` 或 `test/compiler/`，
  不必往已知签名里加东西。

签名格式固定为 `<category>:<16 hex>`，所以同一个程序在改名、重排之后仍然命中同一条。

---

## 8. 滚动 seed（M-7）

每天换一批新程序，且**任何人都能从 finding 里的 seed 重建它**：

```
seed = LE(h[0..8]) mod (2^47 - 1),  h = sha256("<git_sha>:<YYYY-MM-DD>:<counter>")
```

- `git_sha`：工作区 HEAD（环自己取）；`date`：`date +%F`；`counter`：持久化在
  `build/kernfuzz/rolling-counter` 的整数，每次跑完全绿的环就前进本轮用掉的条数。
- 折进 int48 正数域是因为语言没有 bignum，`2^47-1` 让乘法路径不越界（`fast.ta` 的
  `roll_seed` 逐位折，不构造宽整数）。
- fast 与 nightly **共用一个计数器**：两个环从同一条序列里取，nightly 的尾巴不会重复
  push 已经跑过的 seed。
- 同日同 commit 同 counter → 同 seed 块；`test/basic/kernfuzz-fast.ta` 把
  2026-09-04/abc123 这一组数字钉成兼容性锚点，派生逻辑被重构时立刻可见。
- 只想看种子（不跑环）：

```sh
./tinyactor run tools/kernfuzz/fast.ta rolling-seeds --date 2026-09-04 --counter 0 --count 5 --git-sha abc123
```

---

## 9. CI 分层

| job | 触发 | 命令 | 上限 |
|-----|------|------|------|
| `kernfuzz-fast`（`ci.yml`） | 每 push | `make kernfuzz-fast` | `timeout-minutes: 20` |
| `kernfuzz-snapshot-check`（`ci.yml` coverage job） | 每 push | `make kernfuzz-snapshot-check` | 随 job |
| `kernfuzz-nightly`（`kernfuzz-nightly.yml`） | 每日 03:00 UTC + 手工 | 快照门 + `make kernfuzz-nightly` | `timeout-minutes: 90` |

fast 与 nightly 是**同一个入口的两种规模**（§5.2），不是两套编排：nightly 保留三个 TA 环
并把规模推到 1.0、滚动尾巴拉长到 700。P8 之前 nightly 的那四条研究环（golden 全量对拍、
GC 顺序差分、消息多重集、TSan chaos）随生成引擎一起下线，理由见 §11。

工具链缺席时：fast 与 nightly 都印 `KERNFUZZ-SKIPPED <reason>` 并 exit 0——
慢速环也走同一条代码路径，一个每天在 stock runner 上跑的环，缺二进制不是 finding。
（P8 之前的 nightly 在缺工具链时 exit 1；那个分叉随第二套编排一起消失了。）

ASan 臂：Linux CI runner 上 `tavm_asan` 由目标自己构建（`ASAN=1 tavm_asan`）；
macOS arm64 本地没有可用的 ASan 时 summary 会写明 `KERNFUZZ-DEGRADED`，不当红。

### 9.1 实测预算（macOS arm64 2026-11-06，ASan 臂在线）

| 组成 | fast（`--scale 0.4`） | 全量（`--scale 1.0 --count 0`） | nightly（`--scale 1.0 --count 700`） |
|------|------|------|------|
| 1/3 差分 | 200 程序 / 38.6 s | 300 程序 / 55.5 s | 1000 程序 / 213.8 s |
| 2/3 fmt | 200 / 35.3 s | 300 / 52.2 s | 1000 / 186.4 s |
| 3/3 tc 重放 | 567/1418 / 9.7 s | 1418/1418 / 23.7 s | 1418/1418 / 24.2 s |
| 环内合计 | 83.6 s | 131.3 s | 424.4 s |
| `make` 目标墙钟 | 93.8 s | — | 434.5 s |

对照 CI 上限：fast job `timeout-minutes: 20`（实测 1.6 min，约 12 倍余量）；
nightly job `timeout-minutes: 90`（实测 7.2 min，约 12 倍余量）。runner 通常比开发机慢
2–3 倍，仍在一半以内。

---

## 10. 边界与风险

| 风险 | 缓解 |
|------|------|
| `ast-dump` 与 parser 内部结构耦合 | 快照门 + AGENTS.md 的 `parser-ast.ta` 同步义务。注意 issue #67：它 `import` 的是**工作区** `lib/`，可能与入库 `bootstrap.tabc` 不同代 |
| 两条臂共享 VM/GC 的 bug 差分看不见 | ASan 臂（DEC-4）；GC 专项留在 `test/gc/` |
| 生成器自己写错 → 假红 | 生成器只产 `typecheck` 能过的程序；`--self-check`（§5.6）先证明环本身可用 |
| 冻结语料被手滑改写 | 门 = `git status`（§6.3）；生成器先写后扫（§6.3 末段） |
| 计数器丢失 / 回退 | 读不到当 0（新 clone 从序列头开始）；seed 仍可复现，只是重复跑旧批次 |
| hang finding 换个预算就"复现"不了 | 归约器沿用原 finding 的预算（`BUDGET=` 显式传） |
| `tools/kernfuzz/lsan.supp` 无调用方 | 有意保留：它挡的是 Linux 上 LSan 把 by-design 分配报成 leak → `exitcode=42` → B 臂假红（macOS 复现不了）；而无条件接线会让 macOS 的 ASan `failed to parse suppressions` 直接 abort（无 LSan 的运行时不认识 `leak:`）。要接线得先有平台分支——见 §4 |

---

## 11. 历史：P8 收口删掉了什么

P8 之前这个环有三种实现（Guile/Scheme 的参考解释器、Python 3 的生成器/变换/编排、
TA 的新环），18,513 行 Python 与 Scheme 在收口时删除（37 个文件）。判据很简单：
**有没有 TA 接收方**。

| 模块 | 去向 |
|------|------|
| `fast.py` / `nightly.py` | `fast.ta`（三个环）；nightly = 同文件 `--scale 1.0 --count 700` |
| `gen.py` / `prng.py` | `tafuzz_gen.ta`（§5.4 的确定性生成） |
| `morph.py` / `tavm_arm.py` | `tafuzz_arm.ta` + `tafuzz_cmp.ta` + `tafuzz_finding.ta` |
| `reduce.py` / `reduce.sh` | `tafuzz_reduce.ta` + `make kernfuzz-minimize`（§5.5） |
| `tc_meta.py` / `tc_oracle.py` | `fast.ta` 的环 2/环 3；负例**重放**保留，现生成下线 |
| `scratch.py` | `tafuzz_arm.phase_path` + `tools/kernfuzz/build/` |
| `sexp.py` | 砍掉：TA 里 AST 就是值，`ast-dump.ta` 是唯一渲染器 |
| `golden/*.py`（参考解释器 + 其测试） | 砍掉：DEC-1 |
| `multiset.py` / `gc_seqdiff.py` / `gc_workloads.py` / `cps.py` / `transforms.py` | 砍掉：研究环与变体引擎，P8 §3 |
| `test_*.py`（15 个） | 随模块删除；行为由 `tafuzz_check.ta`（21 例）与 `test/basic/kernfuzz-fast.ta` 承接 |

还留在仓库里的、与本环无关的 Python：`tools/coverage_ta.py` 等覆盖率工具（它们服务
`make coverage`，不在本任务范围）。铁律：**kernfuzz 的环不需要任何解释器**。