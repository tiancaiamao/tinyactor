# Spec: TinyActor Phase 2 — 完善

## Goal
为 TinyActor VM 添加 per-process semispace copying GC、string/bytes 内置函数、增强的 C 函数调用机制，确保内存不无限增长、C 交互安全、并通过全面的测试套件验证。

## Background
Phase 1 已完成（2943 行代码，11 个测试全通过）。当前进程死亡时整块释放内存，无 GC。运行期间如果 heap-stack 碰撞，`proc_heap_alloc` 返回 NULL → 值变为 nil → 静默错误。

## Design Decisions (from exploration)

### GC 设计
- **Per-process semispace copying** — 每个进程独立的 from/to space，GC 不影响其他进程
- **触发点**: `proc_heap_alloc()` 中检测到 heap-stack 碰撞时触发
- **Roots**: 栈 (sp→0)、mailbox (所有消息)、gc_roots (C 临时根)、catch_stack
- **Forwarding**: 用 `HeapHeader.flags` bit 0 标记已转发，`type` 字段存 tospace 指针
- **GC 后仍 OOM**: 尝试 realloc 增大 mem → 仍不够则进程崩溃

### C 交互 + GC 耦合
- **`VM->current_proc`**: GC 触发时 C 函数可通过 VM 获取当前进程上下文
- **`Proc->gc_roots[16]`**: 临时根栈，C 函数持 Val 跨越分配点时使用
- **规则**: C 函数应先提取原始数据到 C 变量，再分配；避免持堆指针跨分配
- **HeapUserdata**: 为 C 资源设计的堆对象类型（含可选 finalizer），P1 优先级
- **暂不需要 Registry**: per-process GC + 深拷贝消息 = 无跨进程引用

### 测试策略
- 从 cora 的 49 个测试中提取了 34 个适配测试（按 GC/closure/tail-call/error/actor+GC/string 分类）
- Actor 特有的 GC 测试（GC under send/recv、多进程同时分配）是 cora 没有的
- 按 P0/P1/P2 优先级排列

## Acceptance Criteria

### L1 — Structural
- [ ] 新增文件存在：`src/gc.c`, `src/cfunc.c` — Verify: `ls src/gc.c src/cfunc.c`
- [ ] `make` 成功编译 — Verify: `make clean && make 2>&1`
- [ ] Phase 1 的 11 个测试全部仍然通过 — Verify: 遍历 `test/scripts/*.lisp`

### L2 — GC Correctness
- [ ] **Pair churn**: 大量 cons 分配后回收，内存不无限增长 — Verify: `./tinyactor test/scripts/gc-pair-churn.lisp` 输出 `ok`
- [ ] **Closure churn**: 大量闭包创建+GC，捕获的值正确 — Verify: `./tinyactor test/scripts/gc-closure-churn.lisp` 输出 `ok`
- [ ] **Stack root retention**: GC 后栈上的值不被回收 — Verify: `./tinyactor test/scripts/gc-retains-stack-refs.lisp` 输出 `ok`
- [ ] **Free var retention**: 闭包捕获的自由变量在 GC 后仍可访问 — Verify: `./tinyactor test/scripts/gc-retains-free-vars.lisp` 输出 `ok`
- [ ] **Cross-reference**: pairs 含 closures, closures 含 pairs，GC 后均正确 — Verify: `./tinyactor test/scripts/gc-cross-reference.lisp` 输出 `ok`

### L3 — String/Bytes Operations
- [ ] **string-concat**: `(print (string-concat "hello" " world"))` 输出 `hello world`
- [ ] **string-length**: `(print (string-length "abc"))` 输出 `3`
- [ ] **string-slice**: `(print (string-slice "hello" 1 3))` 输出 `el`
- [ ] **bytes 基础**: 创建和访问 bytes 对象

### L4 — C Function Mechanism
- [ ] `vm_register` 注册的 C 函数可从脚本中调用
- [ ] C 函数参数传递正确，返回值正确
- [ ] C 函数中触发的 GC 不会破坏参数（gc_roots 保护）

### L5 — Actor + GC Integration
- [ ] **GC under send**: 消息传递过程中 GC 正常工作 — Verify: `./tinyactor test/scripts/gc-under-send.lisp`
- [ ] **GC during recv**: 阻塞进程的邮箱消息不被 GC 回收 — Verify: `./tinyactor test/scripts/gc-during-recv.lisp`
- [ ] **Multi-process GC**: 多个进程同时分配+GC 互不干扰 — Verify: `./tinyactor test/scripts/gc-multi-process-stress.lisp`
- [ ] **Ping-pong stress**: 10K 消息 ping-pong + GC — Verify: `./tinyactor test/scripts/actor-ping-pong-stress.lisp`

### L6 — Enhanced Tests (from cora analysis)
- [ ] P0 闭包测试通过: nested capture, curry
- [ ] P0 尾调用测试通过: 5M iterations, mutual recursion
- [ ] P0 错误处理测试通过: crash isolation, monitor DOWN
- [ ] P1 列表操作通过: reverse, map, iterate
- [ ] P1 模式匹配增强通过: pair destructuring

## Constraints
- 纯 C (C99)，无外部依赖
- Per-process GC，不引入全局 stop-the-world
- 不可变赋值 → 无 write barrier 需求
- GC 在 proc_heap_alloc 中触发，C 代码中分配自动参与 GC
- C 函数遵循 "extract-before-allocate" 规范
- 保持 Phase 1 的全部行为不变（11 个测试回归）
- 代码量目标：+~500 行（gc.c ~150 行, cfunc.c ~150 行, 其他修改 ~200 行）

## Out of Scope (Phase 3+)
- try/catch / throw
- 嵌套 pattern matching / guard
- 分代 GC
- 多线程调度
- HeapUserdata finalizer（仅预留类型定义）
- Registry / luaL_ref 式长效引用

## Task Breakdown

### Task 1: GC Core (`src/gc.c`, ~150 lines)
**File scope**: `src/gc.c` (new), `ta.h` (add gc_roots, gc_to_ptr, gc_collect decl), `src/vm.c` (modify proc_new/proc_free for gc_to allocation)

实现 semispace copying collector:
- `gc_collect(Proc *p)`: root scan → copy → scan tospace → swap
- `gc_copy_val(Proc *p, Val *v)`: 如果 v 指向 fromspace，复制对象并更新 Val
- `gc_copy_obj(Proc *p, void *obj)`: 复制单个堆对象，留 forwarding pointer
- `gc_scan_tospace(Proc *p)`: 扫描 tospace 中已复制对象的子引用
- Root scanning: stack (sp→0), mailbox, gc_roots, catch_stack
- Forwarding: flags bit 0 = forwarded, type field = tospace pointer
- GC 后仍 OOM: 尝试 `proc_grow(p)` realloc → 仍不够返回 NULL

**验证**: 编译通过 + Phase 1 测试全部通过（GC 不破坏现有功能）

### Task 2: GC Integration in VM (`src/vm.c`, `src/val.c`)
**File scope**: `src/vm.c`, `src/val.c`, `ta.h`

修改 `proc_heap_alloc` 在返回 NULL 时触发 GC:
- 调用 `gc_collect(p)` 后重试分配
- 如果 GC 后仍不够，尝试 `proc_grow` 扩大内存
- 修改 `val_pair`, `val_string`, `val_bytes` 处理 GC 后重试
- `proc_new`: 分配 gc_to 半空间
- `proc_free`: 释放 gc_to

**验证**: gc-pair-churn, gc-closure-churn, gc-retains-* 测试通过

### Task 3: String/Bytes Builtins (`src/cfunc.c`, ~150 lines)
**File scope**: `src/cfunc.c` (new), `src/compile.c`, `src/vm.c`

- 实现 string 操作: string-length, string-concat, string-slice
- 实现 bytes 操作: bytes-length, bytes-get
- 在编译器中注册为 inline ops 或特殊处理
- 处理 string 在 GC 中的正确复制

**验证**: string-basic-ops, string-gc-stress, string-in-list 测试通过

### Task 4: C Function Call Mechanism (`src/vm.c`, `src/api.c`, `ta.h`)
**File scope**: `src/vm.c`, `src/api.c`, `ta.h`

- 添加 `VM->current_proc` 字段
- 添加 `Proc->gc_roots[16]` 和 `gc_root_count`
- 添加 `gc_root_push(Proc*, Val)` / `gc_root_pop(Proc*)` helpers
- 实现 OP_CCALL 或在 OP_CALL 中处理 cfunc 调度
- `vm_register` 注册的函数可从脚本通过名称调用

**验证**: C 函数测试通过 + gc_roots 在 GC 中正确扫描

### Task 5: Test Suite Integration
**File scope**: `test/scripts/` (新增测试文件)

将 `/tmp/ta-phase2-tests/` 中 34 个测试文件审阅后放入 `test/scripts/`:
- 筛选适用于当前 Phase 2 功能的测试
- 修正测试脚本语法（确保用 TinyActor 已支持的语法）
- 编写测试运行脚本或更新 Makefile
- 确保所有 P0 测试通过，记录 P1/P2 测试状态

**验证**: 所有 P0 测试通过

### Task 6: Final Validation & Cleanup
**File scope**: 全部

- 确认 Phase 1 的 11 个测试全部通过（回归测试）
- 确认 Phase 2 新增的 P0 测试全部通过
- 移除所有 debug 输出 (TRACE fprintf 等)
- 代码审查：无内存泄漏、无未初始化变量
- 更新 .pge/progress.md

## Estimated Code
- `src/gc.c`: ~150 行 (GC core)
- `src/cfunc.c`: ~150 行 (string/bytes builtins)
- `ta.h`: +20 行 (gc_roots, gc_to_ptr, gc_collect, OP_CCALL)
- `src/vm.c`: ~80 行修改 (GC trigger, OP_CCALL, current_proc)
- `src/val.c`: ~20 行修改 (GC-aware allocation)
- `src/api.c`: ~30 行修改 (vm_register dispatch)
- `src/compile.c`: ~30 行修改 (string builtins, OP_CCALL compilation)
- **Total: ~+480 行**