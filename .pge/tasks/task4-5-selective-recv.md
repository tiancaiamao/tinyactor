# Task: Phase 4 Task 5 — Selective Receive

## Context

当前 `(recv)` 是纯 FIFO——取第一条消息。如果收到不想要的消息，没法跳过。

Erlang 的 `receive` 可以扫描邮箱，跳过不匹配的消息，只取匹配的：

```erlang
receive
    {ping, From} -> From ! pong;
    stop -> stopped
end
```

如果邮箱里有 `{data, 42}` 和 `{ping, Pid}`，会跳过 `{data, 42}`，只取 `{ping, Pid}`。

## Goal

为 TinyActor 添加 `(receive ...)` 特殊形式，实现 selective receive：

```lisp
(receive
  (('ping from)  (send from 'pong))
  ('stop         (print "stopping"))
  (msg           (print msg)))  ;; catch-all
```

## Current Implementation

### recv (vm.c ~line 979)
```c
case OP_RECV: {
    if (p->mbox_count == 0) {
        p->pc--;
        p->state = PROC_WAIT_RECV;
        return -1;
    }
    proc_push(p, mbox_pop(p));
    break;
}
```
总是取第一条消息。

### match (compile.c ~line 1005)
`(match expr (pat body...) ...)` — 对一个值做 pattern matching。已有的 pattern 类型：
- `'symbol` — 笹号字面量
- `n` — 数字字面量
- `x` — 变量（总是匹配，绑定）
- `(a . b)` 或 `(a b)` — pair/list 匹配（OP_CAR, OP_CDR, OP_EQ）
- `_` — 通配符

### mailbox 结构 (MsgFragment 链表)
```c
MsgFragment *mbox_frag_head;  // 链表头
MsgFragment *mbox_frag_tail;  // 链表尾
int          mbox_count;
pthread_mutex_t mbox_lock;
```

## Implementation Approach

**推荐方案：编译为 mailbox 扫描循环**

`(receive (pat1 body1) (pat2 body2) ...)` 编译为：

```
RECV_LOOP:
    ; 尝试从邮箱取下一条消息（不阻塞，返回 nil 如果空）
    OP_RECV_PEEK        ; peek 下一条消息，push 到栈上，不删除
    
    ; 检查是否为 nil（邮箱空了）
    DUP
    OP_IS_NIL
    JUMP_IF_TRUE RECV_BLOCK
    
    ; 尝试第一个 pattern
    DUP                 ; 复制消息值
    ; ... pattern1 match code ...
    JUMP_IF_MATCH BODY1
    
    ; 尝试第二个 pattern
    DUP
    ; ... pattern2 match code ...
    JUMP_IF_MATCH BODY2
    
    ; ... 更多 patterns ...
    
    ; 都不匹配 — 消息留在邮箱中，循环
    ; 但 peek 已经"消耗"了这个消息...
    ; 需要一个 OP_RECV_SKIP 把当前 peek 的消息跳过
    
    JUMP RECV_LOOP

RECV_BLOCK:
    ; 邮箱中没有匹配的消息 — 阻塞等待
    OP_RECV_WAIT        ; set PROC_WAIT_RECV, rewind to RECV_LOOP
    JUMP RECV_LOOP

BODY1:
    DROP                ; 丢掉消息值（已经在 pattern 中绑定了变量）
    ; ... body1 code ...
    JUMP END

BODY2:
    ; ... body2 code ...
    JUMP END

END:
```

**关键：需要两个新 opcode**

### OP_RECV_PEEK
```c
case OP_RECV_PEEK: {
    // 查看邮箱中下一个未查看的消息
    // 不从邮箱删除，只是标记"已查看"
    // 如果没有更多消息，push nil
    // 如果有消息，deep_copy 到栈上（用于 pattern matching）
    if (p->peek_index >= p->mbox_count) {
        proc_push(p, val_nil());
    } else {
        // 获取第 peek_index 个 fragment
        // deep_copy 到堆上
        // push 到栈
        p->peek_index++;
    }
    break;
}
```

### OP_RECV_COMMIT / OP_RECV_SKIP
```c
case OP_RECV_COMMIT: {
    // 匹配成功 — 从邮箱中删除第 peek_index-1 个 fragment
    // 重置 peek_index = 0
    break;
}

case OP_RECV_SKIP: {
    // 不匹配 — 继续扫描下一个
    // peek_index 保持，下次 OP_RECV_PEEK 取下一条
    break;
}
```

### OP_RECV_WAIT
```c
case OP_RECV_WAIT: {
    // 重置 peek_index = 0
    // 设置 PROC_WAIT_RECV
    // 回退 PC 到 RECV_LOOP 开始
    p->state = PROC_WAIT_RECV;
    return -1;  // 退出 reduction 循环
    // 当被唤醒时（新消息到达），VM 重新执行 OP_RECV_PEEK
}
```

### Proc 新增字段
```c
int peek_index;  // 当前扫描到的 mailbox 位置（0=从头开始）
```

## Compile.c Changes

在 `compile_expr` 中添加 `receive` 特殊形式处理：

```c
if (sym_eq(c->vm, head, "receive")) {
    // 编译为 mailbox 扫描循环
    // 1. RECV_LOOP: OP_RECV_PEEK
    // 2. 检查 nil → OP_RECV_WAIT
    // 3. 对每个 pattern 编译 match code
    // 4. 匹配 → OP_RECV_COMMIT + body
    // 5. 都不匹配 → JUMP RECV_LOOP
}
```

## Alternative Simpler Approach

如果上面的扫描循环太复杂，可以用更简单的方式：

**把 `(receive ...)` 编译为递归函数：**

```lisp
;; (receive ('ping from) body1) ('stop body2) (msg body3))
;; 编译为：
(let __recv_msg (recv))
(if (match-try __recv_msg 'ping)
    (let from (cadr __recv_msg))
    body1)
    (if (match-try __recv_msg 'stop)
        body2
        body3))
```

但这不是真正的 selective receive（不跳过不匹配的消息）。

**所以推荐用扫描循环方案。**

## Verification

创建 `test/scripts/recv-scan.lisp`：

```lisp
;; Actor A sends 3 messages to Actor B in wrong order
;; B uses selective receive to get them in right order
;; Proves: non-matching messages stay in mailbox

(define (server me)
  ;; Wait for "second" first, then "first"
  (receive
    ('second
     (receive
       ('first
        (send me 'done))
       (msg
        (send me (list 'unexpected msg)))))
    (msg
     ;; Got unexpected message first, try again
     (send me (list 'unexpected msg)))))

(define (main)
  (let me (self))
  (spawn (lambda () (server me)))
  ;; Send "first" then "second"
  (let pid (- me 1))  ;; get server pid somehow
  (send pid 'first)
  (send pid 'second)
  (match (recv)
    ('done (print "PASS"))))
```

Also verify:
```bash
# All existing tests pass
NWORKERS=1 pass=0; for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1; [ $? -eq 0 ] && pass=$((pass+1)); done; echo "ST: PASS=$pass"
NWORKERS=4 pass=0; for f in test/scripts/*.lisp; do timeout 10 ./tinyactor "$f" >/dev/null 2>&1; [ $? -eq 0 ] && pass=$((pass+1)); done; echo "MT: PASS=$pass"
```

## Files to Modify
- `ta.h` — `peek_index` field on Proc, new opcode constants
- `src/compile.c` — `receive` special form compilation
- `src/vm.c` — OP_RECV_PEEK, OP_RECV_COMMIT, OP_RECV_SKIP, OP_RECV_WAIT
- `test/scripts/recv-scan.lisp` — new test

## Important Constraints
1. Existing `(recv)` must still work (backward compatible)
2. GC untouched
3. Single-thread and multi-thread both pass
4. Non-matching messages stay in mailbox (can be received later)
5. Build must pass

## Rules
1. Read before write — grep for existing patterns
2. GC must not be modified
3. Output DONE: <file list> when complete