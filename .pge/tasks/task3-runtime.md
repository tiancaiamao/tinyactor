# Task 3: Runtime — vm.c + api.c + main.c + Makefile

## Outcome
实现 VM 解释器、调度器、C API 和 REPL 入口，使 TinyActor 可以运行脚本。

## Context
- Task 1 完成：`ta.h` + `src/val.c` 已存在（类型系统、NaN-boxing、值操作）
- Task 2 完成：`src/reader.c` + `src/compile.c` 已存在（解析器、编译器）
- 本任务实现字节码解释器和运行时

## Files to Create
1. `src/vm.c` — VM 解释器 + 进程 + 调度器 + Actor 原语 + 内置操作
2. `src/api.c` — C API 实现
3. `src/main.c` — Standalone REPL 入口
4. `Makefile` — 构建系统

## src/vm.c 实现要求 (~400-500 行)

### 核心组件

#### 1. 进程管理
```c
Proc *proc_new(VM *vm, uint8_t *code, int *fn_table, int fn_count);
void proc_free(Proc *p);
```
- 分配 Proc 结构体和初始内存（mem: 4096 字节，mbox: 16 slots）
- 设置初始 pc, sp, fp, heap_ptr

#### 2. 堆分配
```c
void *heap_alloc(Proc *p, int size);
```
- 从 heap_ptr 往上分配，与栈（从高地址往下）相向增长
- 如果空间不足，realloc mem（翻倍）
- Phase 1 不做 GC

#### 3. 调度器
```c
void vm_run(VM *vm);
int vm_step(VM *vm, int max_reductions);
```

调度循环：
```
while (runq 非空) {
    pid = dequeue();
    proc = procs[pid];
    if (proc->state != PROC_RUNNING) continue;

    for (r = 0; r < MAX_REDUCTIONS; r++) {
        op = code[pc++];
        switch (op) { ... }
        // RECV 且邮箱空 → state = WAIT_RECV, break
        // HALT → state = PROC_DEAD, notify watchers, break
    }

    if (state == RUNNING) enqueue(pid);
}
```
- MAX_REDUCTIONS 建议 1000（让无限循环进程每 1000 步让出）
- 进程死亡时通知所有 monitor 它的 watcher

#### 4. 字节码解释器（大 switch）

每个 OP_* 指令的实现：

**栈操作**：
- PUSH_NIL/PUSH_TRUE/PUSH_FALSE：push 对应值
- PUSH_INT8：读 1 字节有符号整数，push
- PUSH_INT：读 8 字节 int64，push
- PUSH_SYM：读 4 字节 symbol 索引，push

**局部变量**：
- LOAD：读 2 字节 offset，push stack[fp + offset]
- STORE：读 2 字节 offset，pop → stack[fp + offset]

**pair**：
- CONS：pop cdr, pop car, 分配 HeapPair，push
- CAR：pop pair, push car
- CDR：pop pair, push cdr

**算术**（pop 2 个 int，计算，push 结果）：
- ADD, SUB, MUL, DIV, MOD
- 除零时进程崩溃

**比较**（pop 2 个值，比较，push val_true/false）：
- EQ：值相等（int=int, sym=sym, nil=nil 等）
- LT, LE：int 比较

**类型判断**（pop 1 值，push val_true/false）：
- IS_NIL, IS_PAIR, IS_INT, IS_STRING, IS_BYTES, IS_PID

**控制流**：
- JUMP：读 4 字节地址，pc = addr
- JUMP_IF_FALSE：读 4 字节地址，pop → if false/nil, jump
- POP：sp--
- DUP：push stack[sp-1]

**函数**：
- CLOSURE：读 fn_id(2B), nfree(1B), [offset(2B)]×nfree → 分配 HeapClosure，push
- CALL nargs(1B)：
  - 保存 ret_addr = pc, old_fp = fp
  - 新 fp 指向参数区底部
  - closure = stack[fp+1]（参数上方）
  - pc = fn_table[closure->entry] 或 closure->entry 直接作偏移？
  - **栈帧布局**：[arg0]...[argN-1] [closure] [ret_addr] [old_fp] ← 新 fp 指向 ret_addr 处
  
  重新设计（参考 design.md 4.6）：
  ```
  调用前栈：... [arg0] [arg1] ... [argN-1]
  CALL 执行后：
  ... [arg0]      ← fp+N+1
      [arg1]
      ...
      [argN-1]    ← fp+2
      [closure]   ← fp+1
      [ret_addr]  ← fp+0   ← 新 fp 指向这里
      [old_fp]              ← sp 向下
  ```
  实际上需要仔细考虑。让我参考 design.md 的栈帧布局。

- TAIL_CALL nargs(1B)：不压 ret_addr 和 old_fp，直接覆盖参数区域
- RET：pop 返回值，恢复 fp 和 pc，push 返回值

**Actor**：
- SPAWN fn_id(2B)：创建新进程，从 fn_table[fn_id] 开始执行，push pid
- SPAWN_CLOS：pop 闭包，创建新进程从闭包 entry 开始执行，push pid
  - 需要把闭包的 free vars 复制到新进程的栈上
- SEND：pop val, pop pid，深拷贝 val 到目标进程邮箱
- RECV：如果邮箱非空，取一条消息 push；否则设 state=WAIT_RECV，break
- SELF：push 当前进程 pid
- MONITOR：pop pid，分配 ref，在目标进程 watchers 注册，push ref

**内建**：
- PRINT：pop 值，格式化输出到 stdout
  - int：直接打印数字
  - symbol：打印符号名（不带 quote）
  - string：打印字符串内容（不带引号）
  - nil：打印 nil
  - pair：打印 (car . cdr) 或列表形式
  - pid：打印 <pid N>
- HALT：进程结束

**匹配**：
- MATCH_INT：读 8 字节 int，比较栈顶（不 pop），匹配则继续，不匹配则 jump 到下一分支
- MATCH_SYM：读 4 字节 sym_idx，类似
- MATCH_NIL：检查栈顶是否 nil
- MATCH_PAIR：检查栈顶是否 pair，匹配则 push car 和 cdr（或绑定到后续 slot）
- MATCH_JUMP：读 4 字节地址，跳到下一分支

#### 5. 内置函数
- 如果编译器将 `+`, `-` 等编译为内联 OP_ADD 等，则不需要函数调用框架
- 如果有 C 注册函数（vm_register），需要在 CALL 时查找 cfuncs 表

#### 6. 深拷贝
```c
Val val_deep_copy(Proc *target, Val v);
```
- int/symbol/nil/true/false/pid → 直接复制 8 字节
- pair → 递归深拷贝 car 和 cdr
- closure → 深拷贝捕获的值
- string/bytes → 复制数据

#### 7. 邮箱操作
```c
void mbox_push(Proc *p, Val msg);  // 消息入邮箱（已深拷贝）
Val mbox_pop(Proc *p);              // 取消息
```

#### 8. 进程死亡通知
```c
void proc_die(Proc *p, VM *vm, Val reason);
```
- 设置 state = PROC_DEAD
- 遍历 watchers，向每个 watcher 发送 ('DOWN ref pid reason)
- 唤醒处于 WAIT_RECV 的 watcher

## src/api.c 实现要求 (~150-200 行)

```c
VM *vm_new(void);           // 分配 VM，初始化符号表、cfuncs 表等
void vm_free(VM *vm);       // 释放所有进程和 VM 资源

int vm_register(VM *vm, const char *name,
                Val (*fn)(VM*, Val*, int), int nargs);

int vm_load(VM *vm, const char *source);   // reader + compile，填充 vm->code 等
int vm_load_file(VM *vm, const char *path);

int vm_spawn(VM *vm, const char *fn_name); // 找到函数，创建进程
void vm_run(VM *vm);                        // 调度循环
int vm_step(VM *vm, int max_reductions);

Val vm_eval(VM *vm, const char *expr);     // 在新进程里执行表达式
```

## src/main.c 实现要求 (~80 行)

Standalone REPL / 脚本执行器。

用法：
```
tinyactor                   # 交互式 REPL
tinyactor script.lisp       # 执行脚本文件
tinyactor -c script.lisp    # 只编译检查语法
echo 'expr' | tinyactor     # 从 stdin 读表达式
```

REPL 模式：
```c
while (read_line()) {
    Val result = vm_eval(vm, line);
    print_value(result);
}
```

脚本模式：
```c
vm_load_file(vm, argv[1]);
vm_spawn(vm, "main");
vm_run(vm);
```

## Makefile

```makefile
CC = cc
CFLAGS = -Wall -Wextra -std=c99 -O2
SRC = src/val.c src/reader.c src/compile.c src/vm.c src/api.c src/main.c
OBJ = $(SRC:.c=.o)

tinyactor: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c ta.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) tinyactor

.PHONY: clean
```

## Critical Design Points

### 栈帧布局（参考 design.md 4.6）
调用前：[arg0] [arg1] ... [argN-1]
CALL nargs 后：
- closure = stack[sp - nargs - 1]（参数下面？不对，参数在栈顶）

实际上需要更仔细地设计。关键：
1. 编译器先编译所有参数（push 到栈上）
2. CALL nargs：
   - 闭包在栈上某个位置（或者编译器知道是全局函数/闭包变量）
   - 保存 ret_addr 和 old_fp
   - 设置新 fp

**建议方案**：
- 编译器对函数调用 `(f a1 a2 ... an)` 生成：
  1. 编译 f（push 闭包或函数引用到栈上）
  2. 编译 a1, a2, ..., an（push 参数）
  3. CALL n
- CALL n 执行时：
  - 栈布局：... [closure] [arg0] [arg1] ... [argN-1] ← sp
  - pop args（sp -= n），找到 closure 在 stack[sp - 1]
  - 保存：push ret_addr, push old_fp → 新 fp = sp - 2
  - 把闭包的 free vars push 到栈上
  - 设置 pc = closure->entry（或 fn_table[closure->entry]）
  - 局部变量通过 LOAD/STORE offset 访问（相对于 fp）

**更简单的方案**（推荐）：
- 编译 `(f a1 a2)` → compile f, compile a1, compile a2, CALL 2
- CALL nargs 时：
  ```
  栈：[closure] [a0] [a1] ... [aN-1]
  新 fp 指向 closure 下方
  [old_fp]    ← fp+0
  [ret_addr]  ← fp+1
  [closure]   ← fp+2
  [a0]        ← fp+3
  [a1]        ← fp+4
  ...
  ```
  实际上让 vm.c 的实现与 compile.c 的期望一致就行。

**核心原则：vm.c 和 compile.c 必须对栈帧布局达成一致。** 由于 compile.c 已由 Task 2 完成，你需要先读取 compile.c 理解其栈帧假设，然后在 vm.c 中实现一致的布局。

## Constraints
- 纯 C (C99)
- 依赖 ta.h, val.c, reader.c, compile.c
- 无外部依赖
- Phase 1 不实现 GC
- 深拷贝必须实现（send 跨进程时）
- 抢占式调度通过 reduction counting
- 进程隔离：一个进程崩溃不影响其他进程

## How to Verify
编译后运行测试脚本：
```bash
make
echo '(+ 1 2)' | ./tinyactor   # 应输出 3
./tinyactor test/scripts/fib.lisp  # 应输出 832040
```

## Reference
- `/Users/genius/project/tinyactor/design.md` — 完整设计
- `/Users/genius/project/tinyactor/ta.h` — 类型定义和 API
- `/Users/genius/project/tinyactor/src/val.c` — 值操作
- `/Users/genius/project/tinyactor/src/reader.c` — 解析器
- `/Users/genius/project/tinyactor/src/compile.c` — 编译器
- `/Users/genius/project/tinyactor/test/scripts/*.lisp` — 测试脚本