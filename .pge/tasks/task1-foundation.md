# Task 1: Foundation — ta.h + val.c

## Outcome
创建 TinyActor 的类型系统和 NaN-boxing 值操作。这是所有其他模块的基础。

## Files to Create
1. `ta.h` — 公共头文件
2. `src/val.c` — NaN-boxing 值操作实现

## ta.h 必须包含
1. **类型定义**：
   - `typedef uint64_t Val;` — 64 位 NaN-boxed 值
   - NaN-boxing tag 常量：TAG_INT(0xFF00), TAG_NIL(0xFF01), TAG_TRUE(0xFF02), TAG_FALSE(0xFF03), TAG_SYM(0xFF04), TAG_PAIR(0xFF05), TAG_PID(0xFF06), TAG_CLOS(0xFF07), TAG_STRING(0xFF08), TAG_BYTES(0xFF09)
   - 堆对象类型：HEAP_PAIR, HEAP_CLOS, HEAP_STRING, HEAP_BYTES
   - HeapHeader, HeapPair, HeapClosure, HeapString, HeapBytes 结构体
   - ProcState 枚举：PROC_RUNNING, PROC_WAIT_RECV, PROC_DEAD
   - Proc 结构体（进程）
   - VM 结构体（调度器）
   - CatchFrame 结构体（Phase 2 预留）

2. **字节码指令枚举**（~40 条）：
   - 栈操作：OP_PUSH_NIL, OP_PUSH_TRUE, OP_PUSH_FALSE, OP_PUSH_INT8, OP_PUSH_INT, OP_PUSH_SYM
   - 局部变量：OP_LOAD, OP_STORE
   - pair：OP_CONS, OP_CAR, OP_CDR
   - 算术：OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD
   - 比较：OP_EQ, OP_LT, OP_LE
   - 类型判断：OP_IS_NIL, OP_IS_PAIR, OP_IS_INT, OP_IS_STRING, OP_IS_BYTES, OP_IS_PID
   - 控制流：OP_JUMP, OP_JUMP_IF_FALSE, OP_POP, OP_DUP
   - 函数：OP_CLOSURE, OP_CALL, OP_TAIL_CALL, OP_RET
   - Actor：OP_SPAWN, OP_SPAWN_CLOS, OP_SEND, OP_RECV, OP_SELF, OP_MONITOR
   - 内建：OP_PRINT, OP_HALT
   - 匹配：OP_MATCH_INT, OP_MATCH_SYM, OP_MATCH_NIL, OP_MATCH_PAIR, OP_MATCH_JUMP

3. **C API 声明**：
   - 生命周期：vm_new, vm_free
   - 加载：vm_register, vm_load, vm_load_file
   - 执行：vm_spawn, vm_run, vm_step
   - REPL：vm_eval
   - 值构造：val_int, val_nil, val_true, val_false, val_symbol, val_pid, val_pair, val_string, val_bytes
   - 值访问：val_is_int, val_get_int, val_is_nil, val_is_pair, val_get_car, val_get_cdr, val_is_string, val_get_string, val_is_pid, val_get_pid

## val.c 必须实现
1. NaN-boxing 编码/解码：
   - 正常 double → 直接存储（判断 isNaN）
   - int48 → TAG_INT + 低 48 位
   - nil/true/false → 固定 tag，无 payload
   - symbol → TAG_SYM + 低 32 位索引
   - pair → TAG_PAIR + 低 48 位指针
   - pid → TAG_PID + 低 32 位 pid
   - closure → TAG_CLOS + 低 48 位指针
   - string → TAG_STRING + 低 48 位指针
   - bytes → TAG_BYTES + 低 48 位指针

2. 值构造函数：val_int, val_nil, val_true, val_false, val_symbol, val_pid

3. 值判断/访问函数：val_is_int, val_get_int, val_is_nil, val_is_pair, val_get_car, val_get_cdr, val_is_string, val_get_string, val_is_pid, val_get_pid, val_is_symbol, val_get_symbol, val_is_true, val_is_clos, val_is_pid_type

4. 堆分配函数：val_pair（需 Proc 指针，在进程堆上分配）, val_string, val_bytes

5. 深拷贝函数：val_deep_copy(Proc *target, Val v) — 将值深拷贝到目标进程堆

## Key Design Points (from design.md)
- NaN-boxing：用 64 位 IEEE 754 double 的 NaN 空间编码所有类型
- 堆对象只在本进程堆上有效，跨进程传递必须深拷贝
- 不可变数据保证无循环引用，深拷贝不需要 visited 表
- Proc 结构体约 450 字节初始大小

## Constraints
- 纯 C (C99)
- 无外部依赖
- ta.h 是单一公共头文件，嵌入时只需 #include "ta.h"
- val.c 中的堆分配函数需要 Proc 指针（进程上下文）
- 在 `src/` 目录下创建 val.c，ta.h 在项目根目录

## Reference
完整设计见 `/Users/genius/project/tinyactor/design.md`，特别是：
- 4.1 值的表示（NaN-boxing）
- 4.2 堆对象结构
- 4.3 进程结构
- 4.4 调度器结构
- 4.5 字节码指令集
- 5. C API