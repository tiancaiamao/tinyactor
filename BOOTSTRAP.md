# TinyActor — 多层自举架构

## 目标

消除重复实现。每一层编译器用自己层级的语言实现，C 只负责最底层核心。

热更新是脚本语言的核心优势（参照 Lua / Erlang），架构必须支持模块级热更新。

## 现状

```
.ta 源码 ──→ reader_ta.c (C, 1133行) ──→ compile.c (C, 1389行) ──→ bytecode
.lisp 源码 ──→ reader.c (C, 214行) ──→ compile.c (C, 1389行) ──→ bytecode

另有一套 .ta 自举编译器（tokenizer.ta + parser.ta + codegen.ta, 共 1408行），
功能是 compile.c 的子集，仅用于离线预编译 .tabc，不参与 runtime。
```

问题：reader_ta.c ↔ tokenizer.ta + parser.ta 重复，compile.c ↔ codegen.ta 重复。

## 目标架构：三层

```
Layer 2: .ta 源码
  │  tokenizer.ta + parser.ta (用 .ta 写)
  │  含 pattern matching desugar
  ▼
Layer 1: Lisp IR (S-expr AST)
  │  codegen.lisp (用 Lisp 写)
  ▼
Layer 0: bytecode
  │  VM (C)
  ▼
  执行
```

每层只依赖下层。每层编译器用自己层级的语言实现。

### Layer 0：C VM（不变）

负责：
- 字节码解释
- actor 调度（spawn/send/recv/monitor）
- per-process GC
- NaN-boxing 值表示
- reader.c（S-expr reader，bootstrap 用，保留）
- 基本模块（buf/str/net/http/file，C 实现）

C 文件清单（最终）：

| 文件 | 职责 | 行数（当前） |
|------|------|-------------|
| vm.c | 解释 + 调度 | ~1000 |
| gc.c | GC | ~400 |
| val.c | NaN-boxing + 类型 | ~300 |
| buf.c | bytes buffer | ~200 |
| str.c | string 操作 | ~200 |
| reader.c | S-expr reader（bootstrap） | 214 |
| file.c / net.c / http.c | 模块 | ~各200 |
| main.c | CLI | ~120 |

reader_ta.c 和 compile.c 降级为 bootstrap-only，最终可精简或移除。

### Layer 1：Lisp IR → bytecode

codegen.lisp，用 Lisp 写。负责：
- literal → PUSH 指令
- symbol → LOAD / CLOSURE 指令
- if / begin / let → 控制流 + slot 管理
- lambda → free var 分析 + OP_CLOSURE
- define → 函数注册 + ENTER
- and / or → 短路求值
- call / tail-call → CALL / TAIL_CALL
- spawn / send / recv / self / monitor → actor 原语
- receive-scan → RECV_PEEK/RECV_COMMIT 循环
- top-level 编排（jump / entry / fn_table）
- .tabc 序列化

**不含**：pattern matching 编译（已在 Layer 2 desugar）。

预估 ~550 行 Lisp。

### Layer 2：.ta → Lisp IR

tokenizer.ta + parser.ta，用 .ta 写。负责：
- 词法分析
- 语法分析 → Lisp AST
- **pattern matching desugar**（match/if/receive 展开）
- ADT 构造器识别

pattern desugar 规则：

```
match scrut { arm1, arm2, ... }
  ↓
(let temp scrut
  (if guard1 (lets1 body1)
    (if guard2 (lets2 body2)
      nil)))

其中 guard 和 bindings 由 pattern desugar 生成：
  _              → guard: true,              bindings: []
  n              → guard: true,              bindings: [(n, temp)]
  42             → guard: (= temp 42),       bindings: []
  "str"          → guard: (str.eq temp "str"), bindings: []
  nil            → guard: (null? temp),      bindings: []
  'sym           → guard: (= temp 'sym),     bindings: []
  (cons pa pb)   → guard: (and (pair? temp) (and ga gb))
                   bindings: ba ++ bb
                   其中 (ga,ba) = desugar(pa, (car temp))
                         (gb,bb) = desugar(pb, (cdr temp))
  [p1 p2 ... pN] → 嵌套 cons 展开，末尾检查 null?

receive { arm1, arm2, ... }
  ↓
(receive-scan
  (lambda (msg)
    (if guard1 (begin (recv-commit) (lets1 body1))
      (if guard2 (begin (recv-commit) (lets2 body2))
        (recv-skip)))))
```

关键前提：codegen.lisp 支持 `and` 短路求值（~15 行），防止 desugar 后 guard 里的 (car temp) 在 (pair? temp) 为 false 时被执行。

## Bootstrap 链

冷启动时，C 提供最小 bootstrap 工具链：

```
Step 1: reader.c 读 codegen.lisp
        → compile.c 编译
        → codegen.tabc（随二进制分发）

Step 2: reader_ta.c 读 tokenizer.ta + parser.ta
        → compile.c 编译
        → tokenizer.tabc + parser.tabc（随二进制分发）
```

编译好的三个 .tabc 打包随二进制分发。

## 模块加载与热更新

### 多模块共存（当前方案：fn_id rebase）

VM 维护全局 code 区域 + 全局 fn_table + 全局符号表。每个 .tabc 模块加载时：

1. code 追加到全局 code 区域（rebase 所有 offset）
2. fn_table 追加（rebase fn_id → offset 映射）
3. 符号注册到全局表：`module.func → global_fn_id`
4. code 里的 fn_id（OP_CLOSURE / OP_SPAWN）统一加 module_base

模块加载后，函数调用统一走 global_fn_id，不需要跨模块间接跳转。VM 调用路径零改动。

### 热更新（远期，参考 BEAM）

当前 rebase 方案不支持热更新。远期实现热更新时，参考 Erlang BEAM：

- 引入两种调用路径：internal call（模块内直接跳）vs external call（查 export 表）
- export 表更新 → 外部调用自动走新版本
- internal call 继续旧版本代码，跑完自然结束
- 保留两个版本（current + old），旧版本无引用后 GC 回收

需要同步改造：codegen 区分内外调用，VM 维护 export 表，闭包区分内部/外部形态。

### AOT 缓存

每个 `.ta` 编译后产出同名 `.tabc`。import 时：
- `.tabc` 存在且比 `.ta` 新 → 直接加载，跳过编译
- 否则 → 走编译管线（tokenizer.tabc → parser.tabc → codegen.tabc），产出新 `.tabc`

### C API

```
vm_load_module(vm, path)         // 加载一个 .tabc 模块（多模块追加 + rebase）
vm_load_bytecode(vm, bytes)      // 从内存 buffer 加载字节码（编译管线用）
```

## Runtime 编译管线

用户跑 `foo.ta` 时，bootstrap driver（Lisp）编排：

```
(import "tokenizer")
(import "parser")
(import "codegen")

(let source (file.read "foo.ta"))
(let toks (tokenizer.tokenize source))
(let ast (parser.parse toks))
(let bc (codegen.compile ast))
(vm.load-bytecode bc)
(vm.spawn-main)
```

main.c 只负责：注册 C 模块 → 加载 bootstrap .tabc → spawn driver。

compile.c 和 reader_ta.c 不参与 runtime。

## 实施步骤

### 阶段 1：codegen.lisp 追平 codegen.ta（低风险）

把 codegen.ta 的逻辑 1:1 搬到 Lisp 语法。验证它能在 reader.c → compile.c 下编译并产出正确 .tabc。

产出：codegen.lisp（基本功能：literal/symbol/call/if/let/begin/define/top-level）。

验证：用现有 .lisp 测试脚本对比字节码输出。

### 阶段 2：补齐 compile.c 的功能

在 codegen.lisp 里逐个补上：

1. and / or 短路求值（~15 行）
2. closure + free var 分析（~100 行）
3. let 多绑定（~30 行）
4. spawn / send / recv / self / monitor（~40 行）
5. tail call（~5 行）
6. receive-scan 特殊形式（~25 行）

注意：语言设计为完全不可变（design.md），不支持 set!。

每补一个，用对应测试脚本验证。

### 阶段 3：parser.ta 加 pattern desugar

在 parser.ta 的 parse_match 里直接产出 desugar 后的 if/let 嵌套，不再产出 (match ...) AST。

同步修改 parse_receive，产出 (receive-scan ...) 形式。

### 阶段 4：VM 多模块加载 + 热更新

改造 vm_load_tabc 为 vm_load_module，支持：
1. 多模块追加（code/fn_table/symbols rebase 合并）
2. 符号注册（module.func → global_fn_id）
3. 模块热更新（原地替换或追加，符号映射更新）
4. vm_load_bytecode API（从内存 buffer 加载，编译管线用）

### 阶段 5：bootstrap driver

编写 Lisp driver 脚本，编排编译管线。main.c 精简为：注册 C 模块 → 加载 .tabc → spawn driver。

### 阶段 6：降级 C 编译器

compile.c 和 reader_ta.c 标记为 bootstrap-only。

runtime 路径完全走 .tabc 管线。

### 阶段 7（可选）：精简 C

- compile.c 精简为最小 bootstrap 编译器（只够编译 codegen.lisp + tokenizer.ta + parser.ta）
- vm.c 移除 MATCH_* 指令（pattern 已 desugar，不再需要）

## 砍掉的东西

| 移除项 | 原因 |
|--------|------|
| codegen.ta | 被 codegen.lisp 替代 |
| MATCH_* 指令（OP_MATCH_INT/NIL/SYM/PAIR/JUMP） | pattern desugar 到 if + and + 谓词 |
| compile.c 里的 pattern 编译（~200 行） | 同上 |
| compile.c 的 runtime 角色 | 降级为 bootstrap-only |

## 不变的东西

| 保留项 | 原因 |
|--------|------|
| VM 核心（vm.c / gc.c / val.c） | Layer 0，永远需要 |
| reader.c | bootstrap 读 S-expr |
| RECV_PEEK / RECV_COMMIT | receive 语义需要 VM 支持 |
| 字节码格式 | 不变 |
| NaN-boxing 值表示 | 不变 |