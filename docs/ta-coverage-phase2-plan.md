# TA Coverage Phase 2 规划：语句 / match-arm 粒度 + 源码行号

状态：已实施（PR 见 git log；gate `make coverage-ta` 报告新增语句级行）
前置：phase 1（#205/#206）——函数入口插桩，`make coverage-ta`，gate `COV_TA_MIN=85`
上位设计：**`docs/coverage-design.md`**（#205 提交，含 Erlang/Go/SBCL/cloverage/racket/
Guile/luacov 先例调研与"机制落在位置信息存活的层级"规则）。本文是该设计 phase 2 节的
落地细化；与上位设计的唯一分歧见 §3.0。

## 1. 目标

回答 phase 1 回答不了的问题：**函数内部的哪些语句、哪些 match/receive 分支没有被执行到**，
且报告能定位到 `文件:行号`。

非目标（明确出界）：

- 表达式级/子表达式级覆盖——不属于本设计定义的 phase 2/3；SBCL 式"进入但未走完"三值
  状态是上位文档定义的 **phase 3**，另立分支
- stdlib `lib/*.ta` 的覆盖测量（测试程序仍不带 `--cov` 编译，维持 phase 1 现状）
- VM 层任何改动（`src/cov.c` 计数器机制原样复用）

## 2. 已核实的代码事实（设计依据）

1. **行号数据已经存在**：`tokenizer.tokenize_pos` 返回 `(tokens . positions)`，每个 token 带
   `(line col off)`。上位文档 phase 2 节同样以此为前提（"the `tokenize_pos` design"）。
2. **`cov.hit` 无需 typecheck 改动**：`cov` 是注册的 builtin module（`src/cov.c:119`
   `vm_register_cov_module`），typecheck 的 `infer_call` 对 builtin module 的未知调用走
   permissive 路径（issue #96 Phase B）。注入的 `(cov.hit k)` 天然通过类型检查。
3. **block 解析函数族是自包含的**：语句只产生于 `parse_braced` / `parse_block_rest` /
   `parse_block_forms` / `parse_match_arms` 四个函数（含 `receive` 复用 `parse_match_arms`）。
   它们互相直接调用，只经过 `parse_form` / `parse_expr` 解析单个语句——位置信息只需穿这
   4 个函数，不必穿透全 parser。
4. **`begin` 是语句容器**：凡花括号 block 都解析为 `(begin s1 s2 ...)`（包括 `if` 分支带
   block 的情况），arm body 是 `(pat guard body)` 三元组的第三个元素。

## 3. 设计

### 3.0 与上位设计的机制分歧（唯一一处，显式记录）

`coverage-design.md` phase 2 草案选**平行 span 表**：parser 产出与语句平行的 tokpos 列表、
AST 形状不变，但 span 列表必须随 `resolve_loop` 的 splice / 钻石依赖 / 环检测机制平行穿线，
且 id 要按模块分段——文档自己标注这是 "phase 1 deliberately avoids" 的复杂度。

本规划改用**标记包装**，且标记直接长成一条 typecheck 天然放行的普通语句：
`(begin (cov.line file line) stmt)`。它对 typecheck 是 `cov.<fn>` 的 permissive builtin 调用
（§2.2），无需任何特殊处理；covinst **留在 phase 1 原槽位**（typecheck 后、codegen 前）把它
原地改写为 `(begin (cov.hit k) stmt)`——管线顺序零改动、签名缓存零顾虑、`resolve_loop`
零改动。`cov.line` 不在 cov 模块注册表（只有 hit/dump/reset）：若 instrument 漏改写，
运行时会大声报错而非静默丢数据——这是想要的失败模式。

判断依据：本项目已踩过 `resolve_loop` 的隐性坑（covinst 撞名静默 nil），平行列表跨
resolve 保持 lockstep 是第二处需要人肉维持的对齐；标记随 form 走，对齐由构造保证，且
AST 形状变化只存在于 `--cov` 构建的 parse→instrument 区间。若实施中触碰预想外阻力，
回退到上位文档的 span 方案，两案共用 §3.4 以后的全部接口（covmap 格式、报告、gate）。

### 3.1 位置标记：`(begin (cov.line file line) stmt)`，仅 `--cov` 构建产生

- `parse_module_content`（driver.ta）在 `--cov` 下把 `poss` 与 `path` 传给 parser 的 block
  函数族；每个语句 / arm body 在构造处包一层 `(begin (cov.line file line) stmt)`，行号取
  语句首 token（arm 取 pattern 首 token）。
- `parser.parse(toks)` 签名不变、行为不变（委托给带 `poss` 的内部路径，`poss=nil` 时不打
  标记）。**非 cov 构建的 parser 输出与现在 byte 级一致**——bootstrap fixed point 不受影响，
  `parser-ast.ta` 文档测试不受影响。
- 不选 VM 层方案（`TokEntry` 加 `line` 字段 + `vm.tok_line`）：编译器层能解决的事不下沉
  VM 层（分层原则第 3 条）。

### 3.2 covinst 原地改写（管线位置不变）

instrument 仍在 phase 1 的槽位（typecheck 之后、codegen 之前），改写规则：

- 每个 top-level `define`：保留 phase 1 的函数入口插桩（k 分配给 entry）。
- 递归走 body：每个 `(begin (cov.line file line) stmt)` → 原地改写为
  `(begin (cov.hit k) stmt)`，记录 `(k, fn, file, line)`。
- `match` / `receive` 的 arm body 同样处理（receive 与 match 共用 arm 形状）。
- **单表达式 body**（非 begin 的 lambda/单语句 fn）：视为一条语句，同样插桩，否则这类函数
  只有 entry 命中、没有语句粒度。
- `cov.hit` 返回 nil、`(begin ...)` 取最后一项的值——语句位置的值本来就被丢弃，语义不变；
  arm body 用 `begin` 包裹同理。

### 3.3 签名缓存：无需改动

标记 `(cov.line ...)` 对 typecheck 是普通 permissive 调用，不改变任何签名/哈希输入；
instrument 仍在原槽位。phase 1 的缓存机制原样工作，本方案没有任何管线/缓存改动。

### 3.4 covmap / 报告格式

- covmap 行从 `k name` 扩为 **`k file:line name`**（采纳上位文档的行格式）：`line=0` 表示
  函数入口，`line>0` 表示语句 / arm。`file` 来自 `parse_module_content` 已有的 `path`。
- `tools/coverage_ta.py`：解析新格式；报告按 file 分组、按 line 排序；MISS 行打印
  `file:line`；头部百分比与 gate 逻辑不变。
- **gate 语义第一版不动**：`COV_TA_MIN` 仍只考核函数入口（`line=0` 的条目）。语句级覆盖
  先进报告、积累基线，是否纳入 gate 留待有数据后决定——避免门禁数值突变阻塞无关 PR。

## 4. 拒绝的备选方案

| 方案 | 拒绝理由 |
|---|---|
| TokVec 存行号 + `vm.tok_line`（VM 层） | 违反分层原则；编译器层 `tokenize_pos` 已有数据 |
| typecheck 容忍标记包装（若标记非 permissive 形式） | 需在 3846 行 typechecker 的多处语句遍历点加解包，复杂度最高——本方案靠标记长成 permissive 语句绕开 |
| 报告期重解析源码定位行号 | covinst 与报告工具两处实现同一 walk，必然漂移 |
| 先上无行号的语句级（结构键 `(fn, idx)`） | 跨模块同名 fn 键冲突；行号已证实可得，covmap/report 格式不值得变更两次 |

上位文档的**平行 span 表**方案不算"拒绝"而是回退备选（见 §3.0）：若标记方案在实施中
触碰预想外阻力，按原方案回退，下游接口不变。

## 5. 实施步骤（单 feature branch，一个 PR）

1. **parser**：block 函数族加 `poss`/`path` 参数 + 标记构造（~50 行，含 `parse(toks)` 兼容委托）。
2. **driver**：`--cov` 下把 `poss` 与 `path` 传给 annotated parse（~10 行，无管线/缓存改动）。
3. **covinst**：instrument 改写规则扩展（entry + 语句 + arm，消费 `(cov.line ...)` 标记，
   产出 `k file:line name` covmap）。
4. **工具**：`tools/coverage_ta.py` 支持新格式 + 分组报告；`test/test_coverage_ta.py` 同步。
5. **测试**：
   - `test/run_cli_tests.sh` 现有 cov 测试扩展：多语句函数 + match arm 的程序，断言
     covmap 带 `file:line`、dump 的语句计数正确（语句命中 > 函数入口命中的场景）。
   - 端到端：`make bootstrap`（两遍 fixed point `cmp`）→ `make test`（0 failures）→
     `make coverage-ta`，人工抽查 report 中 MISS 的 `file:line` 确实是未执行代码。
6. **文档**：`covinst.ta` 头注释更新为 "phase 2 done; phase 3 = SBCL-style three-state"；
   `docs/coverage-design.md` phase 2 节标记为 shipped 并链接本规划；Makefile coverage-ta
   注释同步；不改 `ta-language-spec.md`（无语法变化）。

## 6. 风险

| 风险 | 缓解 |
|---|---|
| instrument 漏改写某个 `(cov.line ...)` 标记 | `cov.line` 未注册，运行时大声报错（fail-loud，非静默） |
| `begin` 语义差异（值语义） | 语句位置值被丢弃；CLI 测试覆盖 arm body 返回值场景 |
| parser 改动波及非 cov 构建 | `parse(toks)` 行为不变 + fixed point `cmp` 把关 |

## 7. 规模估计

parser ~50 行、covinst ~150 行（重写）、driver ~10 行、coverage_ta.py ~40 行、测试 ~60 行。
全部落在编译器层（lib/bootstrap/*.ta + tools/），VM 层零改动。1-2 天含验证。