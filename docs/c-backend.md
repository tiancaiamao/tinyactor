# c-backend — KL 直接编译到 C 的原生后端

与 `docs/backend.md`（字节码后端 tavm）并列的第二条编译管线。同一个 KL 输入
（`.kl` 文本或 `driver.ta` 里 lower 之后的 `List(Top)`）喂两个后端：

```
lower_program ──► List(Top) ──► compile.ta ──► .tabc ──► src/vm.c      （tavm，既有）
                            └─► cgen.ta    ──► .c ──► clang ──► 原生可执行
```

**为什么要有它**：字节码 VM 每条指令都要一次 fetch-decode-dispatch，算术与
分支的固定开销约为原生代码的 2.5–4 倍（见 §8 的实测）。原生后端的价值不是
"再快一点"，而是**把 VM 层的解释开销整个消掉**，同时把 GC / 堆 / 调度 / C 模块
这些已经稳定的部分原样继承下来（`src/rt.h`/`src/rt.c` 只是 vm.c 的宿主契约的
C 对端，不重写 GC）。

**当前范围（M1）**：非 actor 的纯计算 KL 程序。`test/kl/` 的 9 个用例逐字节
对齐字节码后端。actor 原语（spawn/send/recv）、GLOBAL 可写、C 模块调用面
（`cfunc` 之外）在 M2；性能对照与自举在 M3/M4。

## 1. 用法

```sh
# 生成 C 并编译（clang 是硬依赖，见 §4.1）
./tavm -q lib/bootstrap.tabc --emit-c test/kl/fib.kl /tmp/fib.c
clang -O2 -Isrc -I. -o /tmp/fib /tmp/fib.c $(ls src/*.o | grep -v -e vm.o -e main.o -e rt_smoke.o) \
      -lpthread -ldl -lm
/tmp/fib                     # 与 ./tavm 跑同一个 .tabc 的输出逐字节相同

# 回归门（9 个 test/kl 用例：--emit-c → cc → 运行 → cmp .expected + cmp VM）
make -s src/rt.o && sh test/run_kl_c_tests.sh
```

链接式与 `make tac-smoke` 同款：运行时对象 **减 vm.o 减 main.o**（`run_proc` /
`tls_current_proc` / `main` 由 `src/rt.c` 按同一契约重新提供），再加 `rt.o`
与被测生成码。生成码要提供三样东西：`rt_stages[]`、`rt_stage_count`、
`rt_entry_stage`、`rt_init_globals(VM *)`。

## 2. 生成模型：一个 KL 函数 = 一个 stage

```c
typedef Val (*RtFn)(Proc *p, Val a1, Val a2, Val a3, int base, long *budget);
```

- **每个 KL 函数编译成一个 stage**（`rt_st_N`），**每个非尾调用点编译成一个
  续延 stage**。stage 之间一律 `RT_MUSTTAIL return rt_stages[k](...)`，所以
  原生 C 栈恒为 O(1)：尾递归、互相递归、让出都只换 `base` 和 `pc`。
- **续延是 stage 不是闭包**：调用点把"返回谁"写进被调帧的 `RT_CONT`，被调
  `RT_RETURN` 时读出来间接 musttail 回去。零堆分配。
- **帧**（相对 `base`，槽号绝对寻址 `RT_SLOT(p, i)`）：

  | 槽 | 内容 |
  |----|------|
  | `base+RT_SELF`  (0) | self 闭包（自由变量的宿主，GC 根） |
  | `base+RT_CONT`  (1) | 返回目标 stage id |
  | `base+RT_CBASE` (2) | 调用者 base |
  | `base+RT_UNUSED`(3) | 保留 |
  | `base+RT_A1..A3`(4..6) | a1..a3 的"家"槽：**只在让出时落值**，恢复时读回 |
  | `base+RT_ARGS`  (7) | 第 4 个起的形参（溢出区），再往上是本函数的 spill 区 |

  续延位置与形参个数无关（固定偏移），所以**尾调用不需要搬续延**——这比
  字节码 VM 的 `base+n+1..3` 省一次三槽拷贝。
- 非尾调用：被调帧起于 `nb = base + rt_fd`（`rt_fd` = 本 stage 整帧大小，编译期
  常量）。尾调用：`nb = base`，整帧复用。

## 3. 值策略：寄存器传参，槽只接 safepoint

这条是硬约束（不是"所有局部都放槽里"）：

- **前三个实参走 C 实参**（`a1..a3`，即机器寄存器），第 4 个起落 `RT_ARGS`
  溢出区。stage 入口把它们直接搬进 C 局部（`Val v4 = a1;`）。
- **具名绑定 = C 局部 `v<槽号>` + 一个 home 槽**。C 局部是常态；home 槽只在
  **跨 safepoint**（分配/调 C 函数）前后各动一次：spill（写槽）→ safepoint →
  reload（读槽）。不跨 safepoint 的值 LLVM 全程留在寄存器里，槽写读被死代码
  消除。
- **匿名中间值直接落槽**：`if` 的两条分支各写同一个结果槽、`cons` 的两个实参
  先落槽再 `rt_cons_at`。它们在槽里天然跨 GC 安全，省掉一次 spill 决策。
- **常量全内联**（`val_int(1)`、`val_from_double(1.5)`、`rt_const_symbol(...)`）；
  全局函数引用是 `RT_CLOS(stage)`，零堆分配。

## 4. 四条不变量（改动生成器前必须读懂）

### 4.1 musttail 是 ABI，不是优化开关

`RT_MUSTTAIL` = `__attribute__((musttail))`。少了它：① 原生栈不再 O(1)，深
尾递归直接爆栈；② 让出（`RT_YIELD` 里的普通 `return`）会返回到"调用它的那个
stage"并被当作返回值继续执行，语义全错。GCC 无此属性，`src/rt.h` 里有
`#error` 兜底 —— **构建目标固定用 clang**。

### 4.2 `sp` 只深不浅，帧入口整帧填 nil

GC 的根集是 `Proc` 栈的 `[p->sp, 0)`（`src/gc.c` 唯一的根扫描）。`sp` 单调下压
（`RT_PUBLISH` 只取更深者）——一浅一深来回摆动会让祖先帧的槽在中间那次 GC 里
落在根集外，下一帧再推深时它们指向的是上一轮 fromspace 的残骸。

因此 `RT_FILL(p, base, ov, spill)` 把 `[base+RT_UNUSED, base+RT_ARGS)` 和
spill 区**整段填 nil** 再发布 `sp`：分支各自 spill 到不同槽时，合并点上总有
一格在本路径上没写过，那里的残留是上一帧的 Val。填 nil 让"发布整个帧"与路径
无关。（`RT_NIL` 是编译期常量，不是 `val_nil()` 调用——见 §8。）

### 4.3 跨 safepoint 的值必须在槽里，且发布要盖住它们

写头/写实参之前先 `rt_reserve`（= `proc_stack_reserve` 的快路径）：扩栈是
`realloc + memmove`（不是 GC），搬后槽号不变但缓冲区换了地方，**先要地方再
写**；写完把 `sp` 发布到恰好盖住它们，否则被调自己的 `RT_FILL` 扩栈时
memmove 只保留 `[sp,0)`，头会搬丢。

### 4.4 续延 stage 的入口只收 `sp`，不填

被调 `RT_RETURN` 不缩 `sp`，所以续延 stage 的第一句必须是
`RT_SP(p, base, rt_fd - RT_ARGS)`：收回死栈，否则长循环里那块最深的死帧会永远
落在根集里被反复扫描/复制（持续泄漏）。

## 5. 三个只在真跑起来才暴露的生成器坑

1. **`if` 分支的 `let` 必须还原 env**（`gen_if` 的 `set_eb(..., st_eb(st))`）。
   分支里的 `let` 在 C 侧声明在各自的 `{}` 里，合并点若沿用线程化的线性 env，
   之后的 spill 会去写一支从来没绑过的槽号 → `use of undeclared identifier`。
2. **非尾调用前的 spill 用"调用点"的 env，不是"续延跑完"的 env**
   （`spill_slots(eb_slots(st_eb(st1)), ...)`）。`k` 在续延 stage 里新加的
   `let` 只属于那个 stage。
3. **分配/ccall 之前也要 spill，不能只 reload**。最初只在非尾调用处 spill，
   `rt_cons_at` / `rt_ccall_at` / `rt_mkclos` 前没有，于是 reload 读到的是入口
   填的 nil —— `float.kl` 就是这样输出 `(nil ... 2.5)` 的。

## 6. 控制流与调用点速查

- `genv(e, st, k)`：TA-CPS 风格的表达式生成器，`k(St, String)` 收到"求值结果
  的表达式/槽"。
- `gen_tail(e, st)`：尾位置（不产生续延 stage）。
- `gen_call` / `gen_call_sym` / `gen_call_dyn` / `gen_general(_tail)`：
  已判定为函数的直接/动态调用；`gen_cons`/`gen_ccall`/`gen_closure` 是内联的
  分配与 C 函数窗口。
- `call_stage`：非尾调用。它同时负责续延 stage 的生成：`cid` = 续延 stage id
  （写进 `RT_CONT`），`callee` = 即将进入的 stage（musttail / 让出的目标）。
  这两个**不是同一个东西**，早期混用会让非尾调用跳回自己。

## 7. 测试

`test/run_kl_c_tests.sh`（`make test-kl-c`）：每个 `test/kl/*.kl` 走两条管线，
三方对拍 —— 原生输出 == `.expected` == 字节码输出，且报错面分开
（`emit-c` / `cc` / 原生运行 三种失败各自一个名字）。`make tac-smoke` 里的
`test/rt_smoke.c` 是同一批宏的手写可执行规范，cgen 与它必须同输出。

`sanitize`：ASan + UBSan + `TA_GC_STRESS=1` 下 9 个用例全干净。

## 8. 性能（M3 对照数据）

`test/kl/collatz1m.kl`（1..999999 的 collatz 链长最大值 = 纯整数负载，
131.4M 次内层迭代），clang `-O2`，macOS/arm64（m 系列）：

| 实现 | user 时间 | 相对 |
|------|-----------|------|
| 字节码 VM（`./tavm x.tabc`） | 4.12 s | 1× |
| 手写 C 参考（同算法，机器整数） | 0.17 s | 24× |
| **本后端（cgen → clang -O2）** | **1.23 s** | **3.35×** |

两笔关键收益都是"把不必要的工作从热路径上拿掉"，不是调参：

- `RT_FILL` 里的四个 `val_nil()` 是 **extern 函数调用**（`val_nil` 定义在
  `src/val.c`），clang 无法常量传播 → 每次迭代 4 次真调用。改成编译期常量
  `RT_NIL`。
- `RT_FILL` 里的 `proc_stack_reserve` 虽为 `static inline`，但函数体大到 clang
  拒绝内联 → 每次迭代一次真调用。拆成内联快路径（`stack_floor` 下压 + 与堆顶
  gap 检查）+ `rt_reserve_slow` 冷路径。

两者合起来把每步开销从 ~40 周期压到 ~30 周期。与手写 C 的 24× 差距主要是
NaN-boxing 的类型检查/装箱（`rt_add` 等每次都要判 `val_is_int`）——那是值表示
的固有成本，与解释开销无关，也是"字节码达标即止"不再追的依据。

## 9. 未覆盖

- actor 原语（spawn/send/recv）、可写 GLOBAL、`--emit-c` 吃完整 `.ta` 前端。
- 编译期的 C 模块调用面（`cfunc` 已通，其余按 M2 逐个接）。
- 自举（用 cgen 自己编 `driver.ta`）——M4。现阶段两个后端并存，字节码 VM
  仍是默认。