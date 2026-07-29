# TinyActor 自举架构

## 现状：完全自举 ✅

自举固定点已验证：`bootstrap.tabc ≡ bootstrap_selfhost.tabc`

编译器全部用 TA 自身编写，C 只负责 VM 核心 + 内置模块 FFI。

```
.ta 源码 ──→ tokenizer.ta ──→ parser.ta ──→ typecheck.ta ──→ codegen.ta ──→ .tabc bytecode
                 (TA)            (TA)             (TA)              (TA)
                                        ↑ 全部自举
                                        
.tabc ──→ vm.c (C 解释器) ──→ 执行
```

## 架构

```
┌──────────────────────────────────────────────────┐
│             用户代码 (.ta)                        │
├──────────────────────────────────────────────────┤
│  tokenizer.ta  →  parser.ta  →  codegen.ta       │  ← TA 编译器（自举）
│       ↑         typecheck.ta（可选类型检查）       │
├──────────────────────────────────────────────────┤
│                seed: bootstrap.tabc               │  ← 预编译种子
├──────────────────────────────────────────────────┤
│  vm.c (解释器 + 调度器 + GC)  ←  C 运行时          │
│  api.c / buf.c / str.c / file.c / net.c / http.c │
└──────────────────────────────────────────────────┘
```

## 文件清单

### 编译器（全部用 TA 编写）

| 文件 | 行数 | 职责 |
|------|------|------|
| lib/tokenizer.ta | 348 | 词法分析：.ta → token list |
| lib/parser.ta | 1097 | 语法分析 + pattern desugar：token → AST (Lisp IR) |
| lib/codegen.ta | 1628 | 字节码生成：AST → .tabc |
| lib/typecheck.ta | 2027 | Hindley-Milner 类型推断（可选） |
| lib/driver.ta | 198 | 模块解析 + 管线编排 |
| **合计** | **5298** | |

### C 运行时

| 文件 | 行数 | 职责 |
|------|------|------|
| src/vm.c | 1419 | 字节码解释器 + actor 调度器 + 多线程 |
| src/gc.c | 248 | per-process semispace GC |
| src/val.c | 225 | NaN-boxing 值表示 |
| src/api.c | 723 | VM 自省（load_bytecode / parse_source 等） |
| src/buf.c | 201 | 字节缓冲区 |
| src/str.c | 172 | 字符串操作 |
| src/file.c | 76 | 文件 I/O |
| src/net.c | 168 | TCP 网络 |
| src/http.c | 153 | HTTP 解析 |
| src/main.c | 170 | CLI 入口 |
| ta.h | 555 | 公共头文件 |
| **合计** | **~4110** | |

### 已移除的 C 文件

| 文件 | 移除原因 |
|------|----------|
| compile.c | 被 codegen.ta 替代 |
| reader_ta.c | 被 tokenizer.ta + parser.ta 替代 |
| reader.c (Lisp reader) | .lisp 支持已移除 |

## 自举验证

```bash
# 编译 TA 编译器自身
make bootstrap
# → 用当前 tinyactor 编译 driver.ta 生成 bootstrap.tabc

# 验证固定点（自举收敛）
make bootstrap-selfhost
# → 用 bootstrap.tabc 再次编译 driver.ta
# → 输出 bootstrap_selfhost.tabc
# → cmp 确认与 bootstrap.tabc 完全一致
# → FIXED POINT VERIFIED
```

当前状态：

```
$ cmp lib/bootstrap.tabc lib/bootstrap_selfhost.tabc && echo "FIXED POINT VERIFIED"
FIXED POINT VERIFIED
```

## 启动流程

```
main()
  │
  ├─ vm_new() — 初始化 VM
  ├─ vm_register_module() — 注册 C 内置模块
  ├─ vm_load_tabc("lib/bootstrap.tabc") — 加载 TA 编译器
  ├─ vm_spawn(top_fn_id) — 启动 driver.ta 的 main
  │
  │   driver.ta 内部：
  │     vm_get_arg() → 获取源文件路径
  │     file.read() → 读取 .ta 源码
  │     tokenizer.tokenize() → 词法分析
  │     parser.parse() → 语法分析
  │     typecheck.infer_program() → 可选类型检查
  │     codegen.compile() → 生成字节码
  │     vm.load_bytecode() → 加载到 VM
  │     vm.spawn() → 执行
  │
  └─ vm_run() — 进入调度循环
```

## 自举历史

1. **Phase 0**: C 实现 reader + compiler（compile.c / reader_ta.c）
2. **Phase 1**: 用 TA 写 tokenizer.ta + parser.ta + codegen.ta，C 编译器作为种子
3. **Phase 2**: 切换到 TA 路径（./tinyactor file.ta 走 bootstrap.tabc）
4. **Phase 3**: 移除 C 编译器（compile.c 删除）
5. **Phase 4**: 固定点验证通过（bootstrap.tabc == bootstrap_selfhost.tabc）

## 关键设计

### 模块加载

driver.ta 处理 `import` 递归解析：
- 搜索路径：当前目录 → `lib/`
- 支持模块级 `pub` 可见性控制
- C 内置模块通过 `vm.is_builtin_module` 识别

### Pattern Desugar

parser.ta 将 `match` 展开为嵌套 `if` + 谓词调用，消除对 VM 层 MATCH 指令的需求：

```
match x {
  Red -> 1
  Green -> 2
  _ -> 3
}
↓
(let temp x
  (if (= temp 'Red) 1
    (if (= temp 'Green) 2
      3)))
```

### 编译时类型检查

typecheck.ta 实现 Hindley-Milner 类型推断，支持：
- 多态推导（let-polymorphism）
- ADT 构造器 + 模式匹配穷尽性检查
- 函数类型注解验证
- 类型错误报告（宽容模式，不阻塞编译）