# Typecheck 性能分析

## 概述

TinyActor 的 Hindley-Milner 类型检查器在处理大文件时性能严重下降（**优化前状态**）。
`tokenizer.ta`（当时 15 函数）检查只需 0.4 秒，但 `parser.ta`（当时 73 函数）需要 70 秒，
`typecheck.ta` 自身（当时 ~450 函数）在 120 秒超时内无法完成。
**现状**：level-based generalization 落地后 typecheck 不再超时——`make bootstrap`
（含 driver 全量 typecheck）单次约 50s-2min 可完成（AGENTS「Debug Loop」），本文件
「优化后」表也记录 20.9s 完成。

## 实测数据

> 行数/函数数为**优化前当时快照**（历史记录，时间未重跑）；现行 `wc -l`：
> tokenizer.ta 722 行、parser.ta 1621 行、typecheck.ta 4048 行（行首 `fn ` 计数
> 22 / 91 / 176），文件已大幅增长，表中数字不代表现状。

### 优化前

| 文件 | 函数数 | 行数 | typecheck 时间 |
|------|--------|------|---------------|
| math.ta | 2 | 16 | <0.1s |
| tokenizer.ta | 15 | 342 | 0.4s |
| parser.ta | 73 | 999 | 69.6s |
| typecheck.ta | ~450 | 1542 | >120s (超时) |

### 优化后（level-based generalization）

| 文件 | 函数数 | typecheck 时间 | 加速比 |
|------|--------|---------------|--------|
| math.ta | 2 | <0.1s | — |
| tokenizer.ta | 15 | 0.3s | ~1.3x |
| parser.ta | 73 | **6.5s** | **10.7x** |
| typecheck.ta | ~450 | **20.9s** | **>5.7x** (从超时变为完成) |

## 已实现的优化：Level-Based Generalization (commit 605a518)

每个 tvar 增加 `level` 字段，跟踪其绑定深度。`generalize_l()` 只遍历结果类型
(O(T)) 检查 tvar 的 level，替代了原来遍历整个环境的 `free_vars_env()` (O(N×T×|S|))。

### 改动
- tvar 表示：`('tvar id)` → `('tvar id level)`
- `fresh(counter)` → `fresh(counter, level)`
- 新增 `generalize_l()` + `find_generalizable()` — 每次 O(T)
- 所有推断函数增加 `level` 参数
- `let` 绑定在 level+1 推断值，在 level 泛化
- `define` 在 level+1 推断 body（启用正确泛化）

## 根因：O(N³) 复杂度

核心瓶颈在 `generalize()` 函数。每次处理 `define` 时都会调用它，
而它内部调用 `free_vars_env()` 遍历整个环境。

### 调用链分析

```
infer_define → generalize(t, env, s)
  → free_vars_env(env, s)         // 遍历 env 中所有绑定
    → free_vars_scheme(scheme, s)  // 对每个 binding
      → apply_subst(t, s)          // 解析类型变量
        → assq(tvar_id, s)         // O(|S|) 线性搜索
```

在第 k 个 `define` 时：
- 环境有 ~k 个绑定
- 每个绑定的 `free_vars_scheme` 调用 `apply_subst`，做 O(|S|) ≈ O(k) 的 `assq` 查找
- 单步代价：O(k²)

总代价：Σk² (k=1..N) = **O(N³/3)**

### 量化验证

```
parser.ta (N=73):   N³/3 ≈ 130,000 操作
  × 解释器开销 (~10×) × GC 开销 (~5×) = ~6.5M 实际操作
  ≈ 70 秒 ✓ (与实测匹配)

typecheck.ta (N=450): N³/3 ≈ 30,000,000 操作
  × 50× 总开销 ≈ 1.5B 操作
  ≈ ~160 分钟 (远超 120s 超时)
```

## 次要瓶颈

### 1. apply_subst 使用关联列表

每次 `tvar` 查找通过 `assq` 做线性搜索，替换列表大小为 O(N)。
`apply_subst` 在 `unify`、`free_vars_t`、`occur_check` 中都被调用。

代价：每次推断 O(T × N)，总计 O(N² × T)

### 2. list_union 在关联列表上操作

`free_vars_env` 和 `free_vars_t` 都使用 `list_union`，
每次调用 O(|A| × |B|)，在大小为 O(T) 的集合上操作。

总计：O(N² × T²)

### 3. 无路径压缩

替换是不可变的关联列表，没有 union-find 结构。
同一个 tvar 可能被反复解析，没有记忆化。

## 优化方案

### 方案 1：基于层级的泛化（最大收益）——**已实现**（commit 605a518，见上「已实现的优化」节）

**原理**：不用每次计算 `free_vars_env`，而是跟踪每个 tvar 的"绑定层级"。
只有在当前层级创建的 tvar 才需要泛化。完全消除 `free_vars_env` 调用。

**实现**：
- 在 tvar 表示中添加 level 字段
- 进入 `let`/`define` 时增加 level
- `generalize` 只需比较 tvar 的 level

**收益**：O(N³) → O(N² × T)（已兑现，见「优化后」实测表）

### 方案 2：替换表用 Union-Find

**原理**：用不相交集合（disjoint-set）结构替代关联列表，
`apply_subst` 从 O(|S|) 变为近 O(1)（均摊 O(α(N))）。

**挑战**：需要可变状态或不同的表示方式

**预期收益**：O(N²) → O(N × α(N))，对 N=450 额外提速 ~20×

### 方案 3：哈希表环境

**原理**：用哈希表替代关联列表存储环境。
`assq` 从 O(N) 降至 O(1)。

**挑战**：TA VM 当前没有哈希表原语

### 预期效果汇总（规划时记录；方案 1 已实现，未实现的只剩方案 2/3）

| 方案 | 复杂度 | parser.ta | typecheck.ta |
|------|--------|-----------|-------------|
| 当前 | O(N³) | 70s | >120s (超时) |
| +方案1 | O(N²T) | ~5s | ~50s |
| +方案1+2 | O(NT) | <1s | ~3s |

## 结论

typecheck 慢的原因是 `generalize` 函数中 `free_vars_env` 的 O(N³) 复杂度。
"基于层级的泛化"**已实现**（commit 605a518，见上「已实现的优化」），typecheck 已能在
合理时间内完成（`make bootstrap` 约 50s-2min）；进一步的方案 2（union-find）/
方案 3（哈希表）仍待做（见上「优化方案」）。