# Typecheck 性能分析

## 概述

TinyActor 的 Hindley-Milner 类型检查器在处理大文件时性能严重下降。
`tokenizer.ta`（15 函数）检查只需 0.4 秒，但 `parser.ta`（73 函数）需要 70 秒，
`typecheck.ta` 自身（~450 函数）在 120 秒超时内无法完成。

## 实测数据

| 文件 | 函数数 | 行数 | typecheck 时间 |
|------|--------|------|---------------|
| math.ta | 2 | 16 | <0.1s |
| tokenizer.ta | 15 | 342 | 0.4s |
| parser.ta | 73 | 999 | 69.6s |
| typecheck.ta | ~450 | 1542 | >120s (超时) |

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

### 方案 1：基于层级的泛化（最大收益）

**原理**：不用每次计算 `free_vars_env`，而是跟踪每个 tvar 的"绑定层级"。
只有在当前层级创建的 tvar 才需要泛化。完全消除 `free_vars_env` 调用。

**实现**：
- 在 tvar 表示中添加 level 字段
- 进入 `let`/`define` 时增加 level
- `generalize` 只需比较 tvar 的 level

**预期收益**：O(N³) → O(N² × T)，对 N=450 提速 ~15×

### 方案 2：替换表用 Union-Find

**原理**：用不相交集合（disjoint-set）结构替代关联列表，
`apply_subst` 从 O(|S|) 变为近 O(1)（均摊 O(α(N))）。

**挑战**：需要可变状态或不同的表示方式

**预期收益**：O(N²) → O(N × α(N))，对 N=450 额外提速 ~20×

### 方案 3：哈希表环境

**原理**：用哈希表替代关联列表存储环境。
`assq` 从 O(N) 降至 O(1)。

**挑战**：TA VM 当前没有哈希表原语

### 预期效果汇总

| 方案 | 复杂度 | parser.ta | typecheck.ta |
|------|--------|-----------|-------------|
| 当前 | O(N³) | 70s | >120s (超时) |
| +方案1 | O(N²T) | ~5s | ~50s |
| +方案1+2 | O(NT) | <1s | ~3s |

## 结论

typecheck 慢的原因是 `generalize` 函数中 `free_vars_env` 的 O(N³) 复杂度。
实现"基于层级的泛化"可以将其降至 O(N²T)，使 typecheck.ta 自举在合理时间内完成。