# C Module Authoring Guide (E1/E2/E3)

> 目标：**普通人**能自己写 C 模块（不只是调用 C）。
> GC 交互的心智负担 + 安全问题做到 Lua 级别。

TA 的 C 模块 = 一个 `.c` 文件 + 一个 `TaFunc` 导出表 + 注册函数。
不写任何 `ta.h` 以外的代码——所有 API 都在 `ta.h`（唯一公共头）。

## 1. 一分钟看懂：最小模块

```c
// lib/mymod.c  → 编译成 lib/mymod.dylib（或 .so）
#include "ta.h"

// 1) 一个 TA 函数 = 一个 C 函数。args[0..nargs-1] 是调用实参。
static Val my_double(VM *vm, Val *args, int nargs) {
    (void)vm; (void)nargs;
    if (!val_is_int(args[0]))
        return val_int(-1);                 // 错误约定：硬错误（参数非法）→ -1
    return val_int(val_get_int(args[0]) * 2);
}

// 2) 导出表：名字 / 函数 / 参数个数
TaFunc mymod_funcs[] = {
    {"double", my_double, 1},
    {NULL, NULL, 0},
};

// 3) 动态模块入口：dlopen 后 dlsym("vm_load_self") 调用
void vm_load_self(VM *vm) {
    vm_register_module(vm, "mymod", mymod_funcs, 1);
}
```

TA 侧用法（类型签名在 `lib/<mod>.ta` 外部 fn 声明文件，见 §7）——调用
**不需要 `import`**（首次调用运行时懒加载 dylib）；要编译期严格类型检查则
`import <mod>` 加载声明文件：

```ta
print(mymod.double(21))   // 42
print(mymod.double("x"))  // -1（硬错误：参数非法，见 §4）
```

首次调用 `mymod.double` 时运行时自动 `dlopen lib/mymod.dylib` 并注册（编译期
codegen 检测到 `lib/mymod.dylib` 存在即生成懒加载调用；TA 模块无 dylib，
走普通模块解析，互不干扰）。`import mymod` 会加载 `lib/mymod.ta` 声明文件
（若存在），从而让模块调用通过编译期类型检查（见 §7）；无声明文件时
`import` 仍报 "module not found: mymod.ta"。

## 2. Val 类型映射

TA 值 = 64 位 tagged union（`typedef uint64_t Val`）。模块能见到的全部类型：

| TA 类型   | 构造            | 判断          | 读取              |
|-----------|-----------------|---------------|-------------------|
| int       | `val_int(i)`    | `val_is_int`  | `val_get_int`     |
| nil       | `val_nil()`     | `val_is_nil`  | —                 |
| bool      | `val_true()` `val_false()` | `val_is_true` | —    |
| symbol    | `val_symbol(idx)`（`vm_intern_symbol`） | `val_is_symbol` | `vm->symbols[idx]` |
| string    | `val_string(p, s, n)` | `val_is_string` | `val_get_string`（`HeapString*`，`->data`/`->len`） |
| bytes     | `val_bytes(p, b, n)` | `val_is_bytes` | `val_get_bytes`   |
| pair      | `val_pair(p, car, cdr)` | `val_is_pair` | `val_get_car`/`val_get_cdr` |
| pid       | `val_pid(id)`   | `val_is_pid`  | `val_get_pid`     |
| closure   | （一般不构造）  | `val_is_clos` | —                 |

- int 是 **48 位有符号**（符号扩展）。Val 的高 16 位存 tag（`TAG_INT` 等），低 48 位存载荷——不是 NaN boxing；int 实际可用位数为 48，不是文档早期写过的「64 位」。
- float **暂无**。规划形态为堆分配 double（`TAG_FLOAT` 指向堆上的 `double`，与 string/bytes 同一模式，全精度，代价是每次运算一次堆分配）；当前不实现。
- string 是**字节数组**（`char *data` + `int len`，非 NUL 终止语义——用 `len`）。
- symbol 是 VM 符号表的整数索引，`vm_intern_symbol(vm, name)` 可新建/取回。
- **符号表并发**：symbols 是 **VM 级共享数组**，`vm_intern_symbol` **无锁**（线性扫描 + 追加）。调度器是多 worker 线程（`scheduler.c` 的 `workers[]`），运行期动态 intern（如 DOWN/noproc 消息、C 模块调用 `vm_intern_symbol`）在并发首次命中时存在 data race 窗口（重复 strdup 泄漏/数组竞争）。实际风险小（符号多在编译期/加载期集中 intern，运行期命中缓存），但 C 模块应在**初始化阶段**预 intern 所需符号，运行期避免并发新建。

## 3. 分配与 GC 心智模型（E2）

GC 是 **Cheney 半区复制**（per-proc）：存活对象被拷到另一半，堆内绝对指针
（`Val`）随之更新。收集**只发生在分配时**：`proc_heap_alloc` 在堆超过
`gc_trigger` 且 `gc_gate == 0`（当前没有不可根化的活引用）时**就地**收集。VM 主
循环里已经没有任何 GC 检查。

**触发时机**：`proc_heap_alloc` 发现 `heap_ptr > gc_trigger` 时，门开着就当场收集，
门关着则只置 `gc_pending`（**请求**），等门重新开为 0 后由 `proc_gc_drain` 补收。
`gc_trigger` 取**上次存活集的 2 倍**（下限 4 KiB，上限 arena 的 3/4），所以存活集
越大、收集越稀。没有后台 GC 线程，也不会被别的进程触发（每个 Proc 独立 arena，
GC 只收自己的）。

**因此 C 模块不需要任何 root**。GC 的 root 集**只有 TA 栈**，所以「在多次分配之间
于 C 局部持有 `Val` / `char *`」的前提是**这段区间内不发生收集**；运行时用一道
**per-proc 的 `gc_gate` 门**罩住所有无法满足该前提的区段。以前要 `gc_root_push` /
`GC_ROOTS_SCOPE` 的两条理由，现在都被结构性保证了：

1. **C 回调期间不会发生 GC**：`OP_CCALL_NAME` 用 `proc_gc_enter` 把整个 C 调用罩
   在门内，回调里的分配只置 `gc_pending`，要等回调返回、结果压回 TA 栈后
   `proc_gc_reopen` 才补收。所以回调里的 C 局部 `Val` 在整个回调期间都指向同一个
   活对象。
2. **arena 一旦持有对象就不再移动/扩容**：actor 的堆+栈是**固定预留**
   （`TA_ACTOR_HEAP`，默认 64 MiB），首次分配时一次性预留，之后终生不 realloc。
   预留只允许发生在 `heap_ptr == 0`（还没有任何对象）时——空 arena 里不存在指向
   它的指针，搬家对谁都不可见。所以「指向堆内缓冲区的裸指针」（例如解析 HTTP 时
   `char *path_start` 指向某个 HeapString 的 data）在整个回调期间也一直有效。

于是 C 模块可以自由地在多次分配之间持有 `Val`、`char *`——**旧的 `gc_root_push` /
`GC_ROOTS_SCOPE` 机制已删除**（issue #136；门模型与安全点定义见
`design-decisions.md` D11，收集点从「每条 opcode 边界检查」迁到「分配时 + `gc_gate`
门」是 #150）。剩下两条沿用不变：

1. **构造即返回的对象安全**：`val_string`/`val_int`/`val_pair` 构造完就返回的 Val
   永远不需要保护——构造函数内部先分配、后写值。
2. **模块自己 malloc 的东西 GC 不管**：文件描述符、`FILE*`、socket、大缓冲区
   用 int/指针包装，生命周期归模块管（`src/net.c` 的 fd 就是 int 返回）。

唯一的例外形状：**把 `Val` 存进模块自己的静态变量、跨回调使用不成立**。这类值
不在 GC 扫描的位置，半区复制后即失效。需要跨回调/跨进程长期持有的数据请走消息
（`MsgFragment`），不要自己缓存 `Val`。

**调试旋钮**：`TA_GC_STRESS=1` 让每次分配都请求一次收集，把门纪律的破绽从「偶发
悬垂指针」变成确定性崩溃；`test/run_gc_tests.sh` 用它额外跑一轮（GC (stress)）。

**边界**（文档化心智模型）：

| 内存            | 谁管       |
|-----------------|-----------|
| proc arena 里的堆对象（string/pair/bytes/clos） | GC（半区复制；无需 root） |
| actor 的 TA 栈 | GC 的 root（唯一的 root） |
| 模块 malloc 的缓冲区/句柄（fd、FILE*） | 模块自己（TA 侧当 int/opaque） |
| MsgFragment（actor 邮箱） | 运行时（VM 管，不属 proc heap） |

**arena 耗尽是 fatal（不是返回 nil）**：单个 actor 的堆+存活集放不进 arena（或栈
深到与堆相撞而堆已非空）时，tavm 打印占用明细并 `abort()`，绝不返回半个结构让
程序继续跑。调大 `TA_ACTOR_HEAP`（字节数，clamp 到 [4 KiB, 1 GiB]）即可。默认
64 MiB 对常见 actor 富余两个数量级：`test/actor/million-actors.ta` 实测单 actor
峰值 arena 需求 32 KiB（空转 actor 只占 512 B 栈缓冲，首次分配才跳到上限）。

## 4. 错误约定：C 信号词汇表

C 原语层只出**原子信号**，TA 包装层负责 lift 成类型化 API（Result/Option）。
全库统一按 `src/net.c` 的真实 ABI（`net_read`/`net_write`/`net_connect`，
另见 `lib/bufio.ta` 头部注释的 RETRY/EOF 语义印证）：

| 信号 | 含义 | 来源 |
|------|------|------|
| `nil` | **已挂起等待，恢复后需重试**（非错误）——C 侧已 `vm_watch_fd` + `vm_yield`，actor 让出调度，fd 就绪后被重新调度 | net.c 的 EAGAIN 路径 |
| `-1` | 硬错误（syscall errno / 参数非法）；errno 存在失败方 proc-local
  的 `last_errno` 上，经 `net.errno()` 读回（DNS/deadline 路径无 errno） | `net_read` 等 |
| `'eof` | 流结束（仅读路径） | `net_read` 的 `read() == 0` |
| symbol | 可区分失败，仅多阶段操作（connect：`'dns_error` / `'refused` / `'timeout` / `'error`） | `net_connect` |
| 正常值 | 成功（string / fd / 字节数……） | — |

**TA 包装层统一 lift**：`nil` 在 TA 层循环重发（对用户不可见）、
`'eof` → `Option.None`、`-1` → `Err(IoErr(errno))`（经 `net.errno()`）、symbol → `Err(reason)`。已落地示例：
`json.try_parse` 返回 `Result`（`lib/result.ta` / `lib/option.ta`）；
`lib/bufio.ta` 是 nil 重发 + `'eof` 语义的参考实现。低频例外：`print` 等
无失败语义的原语不包。

**新 C 模块一律遵守此词汇表：C 出原子信号，TA 出类型化 API。**不要把
`nil` 当通用失败值返回——`nil` 已被「挂起重试」占用，用它报失败会把
挂起误判成错误（见 `src/net.c` 头部注释的 Return contract）。

## 5. C 可变资源句柄约定

语言层无 opaque type / finalizer，可变 C 资源（buffer / process / tls /
sqlite / sdl 等）按以下约定包装：

- **类型**：单构造 ADT 包装 int 句柄——`type Buffer { Buf(int) }`，纯语言
  现有特性，无语言层改动。
- **跨边界 ABI**：TA 层 match 解包后，**C 一律收裸 int**（与现有 net ABI
  一致）。**防伪造交给 typecheck**：ADT 构造器即能力证明，运行时不校验
  tag——C 侧零感知，简单优先。
- **失效句柄**：已 close 的句柄再操作 → C 按硬错误返回 `-1`，TA 层 lift 成
  `Err("closed")`；**double close 幂等返回成功**。注册表删除即失效。
- **生命周期**：显式 `close`/`free`，责任在调用方（Lua 哲学：fd 有进程退出
  兜底，malloc 型资源靠模块级注册表在 VM 退出时统一释放）。
- **actor 死亡**：v1 接受泄漏（每个模块的文档标注其泄漏面），不引入
  finalizer 机制。
- **语义标注**：buffer / process / tls / sqlite / sdl = **可变句柄**（C 侧
  原地改）；str / list / dict = 值。可变句柄是对 TA 层函数式的明确例外，
  confined 在 C 边界后。

## 6. 注册与加载

| 方式 | 适用 | 做法 |
|------|------|------|
| 静态模块 | 核心运行时 | `void vm_register_<mod>_module(VM *vm)` 在 `src/tavm.c` main 调用；函数表里的名字自动注册成 `"<mod>.<func>"` |
| 动态模块 | 第三方/试验 | 编译成 `lib/<mod>.dylib`；首次调用 `mod.func` 时 `dlopen` + `dlsym("vm_load_self")` 懒加载（vm.c 实现 + codegen 检测 dylib 生成懒加载调用；E3 已落地，参考 `lib/demo.c`） |

动态模块编译（参考 Makefile 的 http 规则）：

```sh
cc -shared -fPIC -I. -o lib/mymod.dylib lib/mymod.c
```

（sanitizer 构建加 `-DTA_MOD_TAG=asan`，产物 `lib/mymod_asan.dylib`，
tavm 自动选与自己匹配的 tag。）

## 7. 类型注册：外部 fn 声明文件（P4 治理）

**现状**：C 模块函数的类型签名由 **外部 fn 声明文件** 提供——每个 C 模块在
`lib/` 下配一个 `<mod>.ta`，一行一个签名：

```ta
// lib/demo.ta — demo C 模块的类型声明
external fn demo.double(int) -> int
external fn demo.greet(string) -> string
external fn demo.pair(int, int) -> pair
```

编译期 `import <mod>` 时 driver 加载该声明文件，typechecker 把 `external fn`
签名注册进类型环境（Pass 0.5 的 `extend_env_from_sigs`），模块调用按声明严格
类型检查；不 `import` 则走宽容的 dotted-call 路径（可调用但无类型承诺）。
**不需要再碰 `lib/bootstrap/typecheck.ta`**——签名不再硬编码在 `make_builtin_env`。

**规则**：
- 加/改 C 模块函数 → 同步更新 `lib/<mod>.ta` 声明文件（签名即文档）
- 声明文件与 C 侧 `TaFunc` 表要保持一致，否则会重现幻影 builtin（typecheck
    承诺、runtime 没有）。声明文件机制确保类型签名有单一权威来源。

## 8. 参考实现

- `src/net.c`（最小、无 GC root、非阻塞 IO + vm_yield 挂起）——**首选模板**
- `src/str.c`（string 模块，含分配）
- `src/http.c`（动态 dylib 模块，最复杂）
- 注册/加载机制：`src/api.c`（vm_register_module / vm_load_c_module）