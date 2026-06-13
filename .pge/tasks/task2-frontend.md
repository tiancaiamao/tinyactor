# Task 2: Frontend — reader.c + compile.c

## Outcome
实现 S-expression 解析器和编译器，将源代码文本编译为字节码。

## Context
- Task 1 已完成，`ta.h` 和 `src/val.c` 已存在
- ta.h 定义了所有类型、NaN-boxing、字节码指令枚举、Proc/VM 结构体
- reader.c 读取 ta.h 中的 Val 类型定义
- compile.c 读取 ta.h 中的 OP_* 指令枚举

## Files to Create
1. `src/reader.c` — S-expression parser
2. `src/compile.c` — S-exp → bytecode compiler

## reader.c 实现要求 (~200 行)

解析 S-expression 文本为嵌套的 Val 结构（pair 列表表示 AST）。

### 需要处理的语言结构
- 整数：`42`, `-7`
- 字符串：`"hello"`
- 符号（symbol）：`'hello`, `'nil`, `'done`（quote 开头）
- 标识符：`x`, `add`, `+`, `-`, `*` 等
- nil：`'nil`
- bool：`#t`, `#f`（或直接用 symbol `true`/`false`？看 design.md... design 里没有显式 bool literal，if 用非 nil 表示 true）
- 括号列表：`(a b c)` → pair 链
- dotted pair：`(a . b)` → pair
- 注释：`;` 到行尾

### 内部接口
```c
// reader.c
typedef struct {
    const char *src;
    int pos;
    VM *vm;
    Proc *proc;  // 用于分配 pair（编译期用临时 proc）
} Reader;

Val reader_read(Reader *r);      // 读一个表达式
Val reader_read_all(Reader *r);  // 读所有表达式，返回列表
```

### 注意
- reader 产生的 AST 是 Val pair 链：`(a b c)` = pair(a, pair(b, pair(c, nil)))
- 特殊形式名用 symbol 标识：'define, 'lambda, 'if, 'begin, 'let, 'match, 'spawn, 'send, 'recv, 'self, 'monitor, 'cons, 'quote

## compile.c 实现要求 (~300-500 行)

将 AST（Val pair 链）编译为字节码。

### 编译目标结构
- 字节码：uint8_t 数组
- 函数表：int 数组，fn_table[i] = 函数 i 的字节码起始偏移
- 全局函数名表：记录 define 定义的函数名和 fn_id 对应关系

### 需要编译的特殊形式
1. **(define (name params...) body...)** — 定义函数，分配 fn_id
2. **(lambda (params...) body...)** — 创建闭包
3. **(if cond then else?)** — 条件，生成 JUMP_IF_FALSE
4. **(begin e1 e2 ... eN)** — 序列
5. **(let ((v1 e1) (v2 e2) ...) body...)** — 局部绑定（编译为 lambda + 调用）
6. **(match expr (pat1 body1) (pat2 body2) ... (_ default))** — 模式匹配
   - Phase 1: int literal, symbol literal, nil, (cons a b), _ 通配符
7. **(spawn fn-name)** 和 **(spawn closure-expr)** — 生成 OP_SPAWN 或 OP_SPAWN_CLOS
8. **(send pid msg)** — 生成 OP_SEND
9. **(recv)** — 生成 OP_RECV
10. **(self)** — 生成 OP_SELF
11. **(monitor pid)** — 生成 OP_MONITOR

### 普通调用
- `(f arg1 arg2 ...)` → 编译参数 + CALL/TAIL_CALL
- 尾位置识别：if 的 then/else 分支、begin 的最后一个表达式、match 的 body、let 的 body

### 模式匹配编译策略
match 编译为：
1. 计算 expr，DUP 保存
2. 对每个 pattern 分支：
   - int/sym/nil：DUP + 匹配比较 + JUMP_IF_FALSE 跳到下一分支
   - (cons a b)：DUP + IS_PAIR + JUMP_IF_FALSE，然后 CAR/CDR 绑定变量
   - _：无条件匹配
3. 匹配成功：POP 掉剩余值，执行 body，JUMP 到 match 结束
4. 所有分支失败：POP，报错

### 内部接口
```c
typedef struct {
    uint8_t *code;
    int code_cap, code_len;
    int *fn_table;
    int fn_cap, fn_count;
    // 全局函数名映射
    char **fn_names;
    int *fn_ids;
    int fn_name_count;
    VM *vm;
} Compiler;

// 编译一组顶层表达式
// 返回 0 成功，填充 vm 的 code, fn_table, fn_count, fn_names
int compile(Compiler *c, Val ast_list);
```

### 内置函数映射
这些不是特殊形式，编译为普通 CALL：
- `+`, `-`, `*`, `/`, `%` → OP_ADD 等（或者编译为内置 CALL？看 design...）
  
  实际上，算术/比较等在 design.md 里列为"内置函数"，但在字节码里有专门的 OP_ADD, OP_SUB 等。
  **编译策略**：当函数名是 `+`, `-`, `*`, `/`, `%`, `=`, `<`, `>`, `<=`, `>=` 时，编译为内联指令（OP_ADD 等），不走 CALL 路径。
  其他标识符调用走 CALL 路径。

  同理：`cons`, `car`, `cdr`, `null?`, `pair?`, `int?`, `string?`, `bytes?`, `pid?`, `print` 编译为内联指令。

### 关键：尾调用识别
在以下位置的表达式是尾位置：
- 函数体（define body 的最后一个表达式）
- begin 的最后一个表达式
- if 的 then/else 分支
- let 的 body 最后一个表达式
- match 每个 pattern 的 body

尾位置的函数调用编译为 TAIL_CALL 而非 CALL。

## Constraints
- 纯 C (C99)
- 依赖 ta.h 的类型和 OP_* 枚举
- 依赖 val.c 的值操作函数
- reader 需要临时 Proc 来分配 pair（编译期创建临时 proc，编译完后释放）
- 编译器输出写入 VM 结构体的 code/fn_table 字段

## Reference
完整设计见 `/Users/genius/project/tinyactor/design.md`，特别是：
- 3.1 语法
- 3.2 完整示例
- 4.5 字节码指令集
- 4.6 栈帧布局
- test/scripts/*.lisp — 所有测试脚本