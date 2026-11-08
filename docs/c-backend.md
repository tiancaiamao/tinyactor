# c-backend — KL 直接编译到 C 的原生后端（M2：寄存器传参 ABI）

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

**当前范围（M2）**：非 actor 的纯计算 KL 程序。`test/kl/` 的 12 个用例在
普通与 `TA_GC_STRESS=1` 两种模式下都逐字节对齐字节码后端。M2 换掉了 M1 的
"实参落槽 + 每帧 7 格"ABI：前三个实参走寄存器，帧头从 7 格缩到 3 格，续延存
函数指针（不查表）。actor 原语（spawn/send/recv）、GLOBAL 可写、C 模块调用面
（`cfunc` 之外）、根集精度在 M3；自举在 M4。

## 1. 用法

```sh
# 生成 C 并编译（clang 是硬依赖，见 §4.1）
./tavm -q lib/bootstrap.tabc --emit-c test/kl/fib.kl /tmp/fib.c
clang -O2 -Isrc -I. -o /tmp/fib /tmp/fib.c $(ls src/*.o | grep -v -e vm.o -e main.o) \
      -lpthread -ldl -lm
/tmp/fib                     # 与 ./tavm 跑同一个 .tabc 的输出逐字节相同

# 回归门（12 个 test/kl 用例，每个跑两遍：普通 + TA_GC_STRESS=1）
make -s src/rt.o && sh test/run_kl_c_tests.sh     # = make test-kl-c
```

链接式：运行时对象 **减 vm.o 减 main.o**（`run_proc` / `tls_current_proc` /
`main` 由 `src/rt.c` 按同一契约重新提供），再加 `rt.o` 与被测生成码。生成码要
提供：`rt_stages[]`、`rt_stage_count`、`rt_resume[]`（每个 stage 的实参恢复格
偏移 = `RT_SPILL + ov`）、`rt_entry_stage`、`rt_init_globals(VM *)`。

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
  | `base+RT_CONT`  (1) | 返回目标 = 续延 stage 的**函数指针**（INT tag 包装） |
  | `base+RT_CBASE` (2) | 调用者 base |
  | `base+RT_SPILL` (3) | 第 4 个起的形参（溢出区，`ov` 个），再往上是本函数的 spill 区 |

  整帧 `fd = RT_SPILL + ov + nspill`（三个分量都是编译期常量）。续延位置与
  形参个数无关（固定偏移），所以**尾调用不需要搬续延**——这比字节码 VM 的
  `base+n+1..3` 省一次三槽拷贝。续延存函数指针而不是 stage id：返回是 2 次
  装载 + 间接 musttail，不进 `rt_stages[]` 查表。

  这三格之外还有 3 格"恢复格"（`[base+RT_SPILL+ov, +3)`），只在让出时承载
  a1..a3 —— 被调 stage 的入口还没跑，寄存器实参已经没地方存了，只能落槽；
  `rt_resume[stage]` 给出同一个偏移，`run_proc` 唤醒时照它读回 C 实参。
- 非尾调用：被调帧起于 `nb = base + rt_fd`（`rt_fd` = 本 stage 整帧大小，编译期
  常量）。尾调用：`nb = base`，整帧复用。

## 3. 值策略：寄存器传参，槽只接 safepoint

这条是硬约束（不是"所有局部都放槽里"）：

- **前三个实参走 C 实参**（`a1..a3`，即机器寄存器），第 4 个起落
  `RT_SPILL` 溢出区。stage 入口把它们直接搬进 C 局部（`Val v4 = a1;`、
  `Val v5 = RT_SLOT(p, base + 4);`）。
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

因此 `RT_ENTER_FRAME(p, base, fd, ov, a1, a2, a3)` 在快路径上依次：下压
`stack_floor` → gap 检查（不过才转 `rt_enter_slow`，那里把寄存器实参先落到
恢复格再扩栈）→ `rt_fill_frame` 把 `[base+RT_SPILL+ov, base+fd)` **整段填
nil** → `RT_PUBLISH` 到 `base+fd`。填 nil 的理由：分支各自 spill 到不同槽时，
合并点上总有一格在本路径上没写过，那里的残留是上一帧的 Val（可能指向已经压缩
掉的 fromspace）。填 nil 让"发布整个帧"与路径无关，代价是每帧 `nspill` 次存
（clang 把这几个常量边界的循环完全展开成 store）。（`RT_NIL` 是编译期常量，
不是 `val_nil()` 调用——见 §8。）

`rt_fill_frame` 必须按 `RT_SLOT` 循环写，**不能**改成"算一次槽指针再递增"：
后者在 fib 上把 clang 的寄存器压力顶爆（0x100 的栈帧 + 每次调用重算基址），
实测 16.7ms → 28.2ms。这是本文件里唯一一条"看起来等价、实测两倍慢"的坑。

### 4.3 跨 safepoint 的值必须在槽里，且发布要盖住它们

写头/写实参之前先 `rt_reserve`（= `proc_stack_reserve` 的快路径，只在
`stack_floor` 真正下压时做 gap 检查）：扩栈是 `realloc + memmove`（不是 GC），
搬后槽号不变但缓冲区换了地方，**先要地方再写**；写完把 `sp` 发布到恰好盖住
它们，否则被调自己的 `RT_ENTER_FRAME` 扩栈时 memmove 只保留 `[sp,0)`，头会
搬丢。多要的 3 格是让出恢复点的空间（见 `RT_YIELD_CALL`）。

### 4.4 续延 stage 的入口只收 `sp`，不填

被调 `RT_RETURN` 不缩 `sp`，所以续延 stage 的第一句必须是
`RT_SP(p, base, rt_fd)`：收回死栈，否则长循环里那块最深的死帧会永远落在根集
里被反复扫描/复制（持续泄漏）。续延 stage 不跑 `RT_ENTER_FRAME`——它接管的是
一个已经发布过的帧，不需要再填一次 nil 或再要一次空间。

## 5. 五个只在真跑起来才暴露的生成器坑

1. **`if` 分支的 `let` 必须还原 env**（`gen_if` 的 `set_eb(..., st_eb(st))`）。
   分支里的 `let` 在 C 侧声明在各自的 `{}` 里，合并点若沿用线程化的线性 env，
   之后的 spill 会去写一支从来没绑过的槽号 → `use of undeclared identifier`。
2. **非尾调用前的 spill 用"调用点"的 env，不是"续延跑完"的 env**
   （`spill_slots(eb_slots(st_eb(st1)), ...)`）。`k` 在续延 stage 里新加的
   `let` 只属于那个 stage。
3. **分配/ccall 之前也要 spill，不能只 reload**。最初只在非尾调用处 spill，
   `rt_cons_at` / `rt_ccall_at` / `rt_mkclos` 前没有，于是 reload 读到的是入口
   填的 nil —— `float.kl` 就是这样输出 `(nil ... 2.5)` 的。
4. **让出的实参必须写进被调的恢复格，不是本帧的溢出区**
   （`RT_YIELD_CALL(p, sid, nb, ov, x1, x2, x3)` 写 `[nb+RT_SPILL+ov, +3)`）。
   唤醒时 `run_proc` 按 `rt_resume[p->pc]` 读的是**即将进入的那个 stage** 的
   偏移；写成调用者自己的槽，恢复出来的实参就是上一次的值。
5. **`rt_resume[]` 必须与 cgen 的 `rt_fd` 同一口径**：`RT_SPILL + ov`。两者
   差一格，症状不是崩溃而是"唤醒后第一次调用拿到错参数"，只有
   `collatz1m` 这种跑满 100 万次让出的用例才会稳定暴露。

## 6. 控制流与调用点速查

- `genv(e, st, k)`：TA-CPS 风格的表达式生成器，`k(St, String)` 收到"求值结果
  的表达式/槽"。
- `gen_tail(e, st)`：尾位置（不产生续延 stage）。
- `gen_call` / `gen_call_sym` / `gen_call_dyn` / `gen_general(_tail)`：
  已判定为函数的直接/动态调用；`gen_cons`/`gen_ccall`/`gen_closure` 是内联的
  分配与 C 函数窗口；`gen_builtin` 是 actor 原语（`self`/`spawn`/`send`/
  `recv`/`monitor`/`recv_after`/`recv_peek`/`recv_commit`，名字→编号在编译期
  固化成 `rt_actor_at(p, RTB_x, ...)` 的实参，运行期零按名查找）。`gen_ccall`
  与 `gen_builtin` 同形：`add_site` 拿一个 kind 2 的 stage id → 实参落槽 →
  标签 → `rt_ccall_at`/`rt_actor_at` → `RT_BLOCKED` 检查 → 结果写槽（§2 的
  阻塞站形状）。
- 调用点的宏序列（顺序不是可选的，见 §4.3）：`spill 活槽` →
  `RT_LINK`（非尾）/ `RT_TAIL_ARGS`（尾）→ `reload 活槽` → 溢出实参 →
  `if (*bg <= 0) RT_YIELD_CALL(...)` → `RT_MUSTTAIL return`。整段包在 `{}` 里，
  为了把 `nb`/`rt_fd` 这些 C 局部限制在调用点作用域。
- `call_stage`：非尾调用。它同时负责续延 stage 的生成：`cid` = 续延 stage id
  （写进 `RT_CONT`），`callee` = 即将进入的 stage（musttail / 让出的目标）。
  这两个**不是同一个东西**，早期混用会让非尾调用跳回自己。

## 7. 测试

`test/run_kl_c_tests.sh`（`make test-kl-c`）：每个 `test/kl/*.kl` 走两条管线，
三方对拍 —— 原生输出 == `.expected` == 字节码输出，且报错面分开
（`emit-c` / `cc` / 原生运行 三种失败各自一个名字）。

每个程序跑**两遍**：普通一遍 + `TA_GC_STRESS=1`（每次堆分配都请求一次收集）
一遍。第二遍是 §4.2/§4.3 唯一的硬门 —— 一次收集就搬走并压缩整个活集，任何
一格没铺 nil、或发布得比写的早，那里的残留旧 Val 会当场变成悬垂指针。

15 个用例覆盖的面：非尾递归与续延（`fib`）、百万次尾调用与让出/唤醒
（`collatz1m`）、深非尾递归 10 万层 + 每层分配（`deeprec`）、溢出形参与非尾
多实参调用（`args6`）、逃逸闭包的自由变量（`cloesc`/`closure`）、分配跨
safepoint 的链表（`list`/`map`）、模式匹配（`match`）、浮点与 `=` 的数值等价
（`float`/`collatz`）、actor 原语与阻塞站（`actor-pingpong` =
spawn/send/recv/self + 消息深拷贝，`actor-recv-loop` = 同一 recv 站点重复
挂起/重入 100 次，`actor-cfunc-block` = `time.sleep` 的 cfunc 站点与 recv 站点
在同一次运行里交替挂起）。


外部验证：ASan（`-fsanitize=address`, `-O1 -g`）+ `TA_GC_STRESS=1` 下 9 个用例
全干净（`deeprec`/`args6`/`cloesc` 已在内）；`make test`（gcc 构建的字节码
VM 全套 327 项）与 `make bootstrap` 均通过，说明 C 后端没有污染既有管线。

手写的 ABI 孪生程序（`test/rt_smoke.c` + `make tac-smoke`）在 M2 里已删除：
"生成形状"由 `--emit-c` 直接产出，同一批机制由上面这批 `.kl` 的生成代码覆盖，
再维护一份手写副本只会与 cgen 漂移（一个设计一个说法）。

## 8. 性能

clang `-O2`，macOS/arm64（m 系列），每个数字是 best-of-5 的墙钟（同一台机器、
同一次会话内测，交叉测量以抵消漂移）。

**整数负载**：`test/kl/collatz1m.kl`（1..999999 的最长 collatz 链 = 131.4M 次
内层迭代）：

| 实现 | 时间 | 相对 tavm |
|------|------|-----------|
| 字节码 VM（`./tavm x.tabc`） | 4.47 s | 1× |
| **本后端（cgen → clang -O2）** | **1.16 s** | **3.9×** |

**调用密集型负载**：`fib`（纯非尾递归，最坏情况 —— 每次调用都要走
`RT_LINK` + 填帧 + 发布 + 让出检查 + 间接返回）：

| 实现 | fib(30) | fib(35) | 说明 |
|------|---------|---------|------|
| 字节码 VM | — | 503 ms | 1× |
| **本后端** | **~16 ms** | **180 ms** | 2.8× tavm |
| 手写 ABI PoC（`/tmp` 里的 `fib_abi.c`） | 8.6 ms | — | 同 ABI、同 GC 安全，但没有 reserve/fill/publish |
| minic（另一门语言的对照基线） | 6.6 ms | 74 ms | 无 GC 安全检查 |

`fib` 上离 minic 还有 ~2.4×，这不是调参能补的，逐项消融（best-of-7，fib(30)，
16.7ms 基线）给出的账：

| 拿掉什么 | 剩下 | 省 |
|----------|------|-----|
| 让出检查（`*bg <= 0` + 恢复格写） | 15.9 ms | 0.8 |
| 填帧（`rt_fill_frame`） | 14.8 ms | 1.9 |
| 扩栈预留（`rt_reserve`） | 14.9 ms | 1.8 |
| 整个帧入口（含发布） | 14.1 ms | 2.6 |

全拿掉仍有 ~11.5 ms —— 也就是**机器开销只占 ~30%，其余是 C 后端的"每帧三格
头 + 续延结果落槽 + 尾调用搬实参"这套 body 形状**。

这四笔的成因**不是同一个**（别把账都记在"根集精度"上）：

| 这笔 | 真正的成因 | 精确根集能省吗 |
|------|-----------|----------------|
| 填帧（铺 nil） | 根集是 `[sp,0)` 全格，死格也必须是合法 Val | 能，只填位图里的活格 |
| 发布（`RT_PUBLISH`） | GC 可在任意分配点跑，栈深度必须已覆盖活值 | 省不掉（仍要一次写），位图只把范围变准 |
| 让出检查（`*bg <= 0`） | 协作式预算/让出，VM 也有同一笔 | 不能 |
| 扩栈预留（`rt_reserve`） | heap 与栈在**同一块 semispace** 里相向生长，开新帧前必须确认不相撞 | 不能 |

要抹掉它们只有一条路：把根集从"深度"换成"精度"—— 每帧的指针图（或影子栈），
让 GC 只扫真正活着的槽。那是 M3 的活，不是 M2 的调参。**M2 的定位因此是：
在"完全 GC 安全"这个前提下能到的形状**，而不是 minic 级的裸速；`collatz` 这种
分配少、算术密的负载已经 3.9× tavm，够支撑自举。

## 9. 未覆盖

- 顶层 `(let …)` 引入的绑定**不跨 `define` 共享**。两条腿都拒绝
  （`reject: undefined name(s) in define`，`compile.ta` 的 `check_names`），
  不是 C 后端的缺口；C 后端的差别只是它连"理论上能访问"都不成立（每个
  `define` 是独立 C 过程，没有共享 entry 帧）。TA 语言里没有可变全局：
  `define` 的右值必须是 lambda，操作码 `LOP_GLOBAL` 是**函数引用**
  （→ `TAG_CLOS_ID` / `TAG_NATIVE`），不是变量存储。
- **`cfunc` 作为值**（把 `str.to_int` 这种裸 cfunc 名当值传/存 —— 调用点
  `(str.to_int s)` 是通的，作值还没有函数指针包装）。这也是语料里仅剩的 2 个
  `EMIT-C`：`kernfuzz-fast` / `kernfuzz-ta-generator`。
- `--emit-c` 吃完整 `.ta` 前端（现在吃 `.kl`；`.ta` 走 `--emit-kl` 再 `--emit-c`
  两步，语料 106 项就是这样跑的）。
- **GC 根集精度**：现在按 `Proc` 栈深度一刀切 —— 收根不复杂（根就是我们自己的
  槽数组，`gc.c` 一个循环扫 `[sp,0)`），缺的是"哪些格活着"这个信息：cgen 只
  维护槽水位 `next_slot`，没有按 safepoint 的活跃集。精确根集 = 每个 safepoint
  一张位图（死格不必是合法 Val、保守保活消失），能省的是 §8 表里的"填帧"与
  堆压力，省不掉"发布/让出/预留"三笔。M3 的活。
- 自举（用 cgen 自己编 `driver.ta`）——M4。现阶段两个后端并存，字节码 VM
  仍是默认。已验证的部分：`--emit-c` 出的 C 编成编译器后重编 `driver.ta`，
  `lib/bootstrap.tabc` 与 `driver.kl` 逐字节相同；但默认管线还没换成它。
  M4 之前还差一层 CLI 对齐：`-q` 之类只由 `src/main.c` 消费、`rt.c` 的 main
  不认识，所以 C 版编译器收到 `-q` 会把它当子命令（fallback 到
  `compile_ta("-q", ...)`，报 "cannot read source file"）。自举版必须补上。
