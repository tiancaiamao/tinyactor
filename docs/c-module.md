# C Module Authoring Guide (E1/E2/E3)

> 目标：**普通人**能自己写 C 模块（不只是调用 C）。
> GC 交互的心智负担 + 安全问题做到 Lua 级别。

TA 的 C 模块 = 一个 `.c` 文件 + 一个 `TaFunc` 导出表 + 注册函数。
不写任何 `ta.h` 以外的代码——所有 API 都在 `ta.h`（唯一公共头）。

> **Historical example warning:** §1's `-1` generic-failure example conflicts with
> the current signal vocabulary in [`stdlib-port-plan.md`](stdlib-port-plan.md#贯穿约定v5-新增先于一切模块); that vocabulary is authoritative. The plan tracks this doc for upstream correction.


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
**不需要 `import`**（装载 .tabc 时 link_unit 自动 dlopen dylib）；要编译期严格类型检查则
`import <mod>` 加载声明文件：

```ta
print(mymod.double(21))   // 42
print(mymod.double("x"))  // -1（硬错误：参数非法，见 §4）
```

装载 `.tabc` 时（link_unit）自动 `dlopen lib/mymod.dylib` 并注册，一次性解析成
cfunc 下标供调用期直接使用；TA 模块无 dylib，走普通模块解析，互不干扰。`import mymod` 会加载 `lib/mymod.ta` 声明文件
（若存在），从而让模块调用通过编译期类型检查（见 §7）；无声明文件时
`import` 仍报 "module not found: mymod.ta"。

## 2. Val 类型映射

TA 值 = 64 位 **NaN-boxing**（`typedef uint64_t Val`）。模块能见到的全部类型：

| TA 类型   | 构造            | 判断          | 读取              |
|-----------|-----------------|---------------|-------------------|
| int       | `val_int(i)`    | `val_is_int`  | `val_get_int`     |
| float     | `val_float(d)`  | `val_is_float` | `val_get_float` / `val_to_double`（int 操作数会加宽成 double） |
| nil       | `val_nil()`     | `val_is_nil`  | —                 |
| bool      | `val_true()` `val_false()` | `val_is_true`（= 非 nil 且非 false） | —    |
| symbol    | `val_symbol(idx)`（`vm_intern_symbol`） | `val_is_symbol` | `vm_symbol_name(vm, idx)`（返回名字串；越界 NULL） |
| string    | `val_string(p, s, n)` | `val_is_string` | `val_get_string`（`HeapString*`，`->data`/`->len`） |
| bytes     | `val_bytes(p, b, n)` | `val_is_bytes` | `val_get_bytes`   |
| pair      | `val_pair(p, car, cdr)` | `val_is_pair` | `val_get_car`/`val_get_cdr` |
| pid       | `val_pid(id)`   | `val_is_pid`  | `val_get_pid`     |
| closure   | （一般不构造）  | `val_is_clos` | —                 |

NaN-boxing 布局（`ta.h` 顶部 + `ta_inline.h`，写 C 模块需要知道的全部）：

- **装箱规则**：一个 Val 就是任意 64 位位模式。高 16 位（bits [63:48]）落入
  negative-NaN 标签区（`TAG_FIRST` 0xFFF1 – `TAG_LAST` 0xFFFB：`TAG_INT` /
  `TAG_NIL` / `TAG_TRUE` / `TAG_FALSE` / `TAG_SYM` / `TAG_PAIR` / `TAG_PID` /
  `TAG_CLOS` / `TAG_STRING` / `TAG_BYTES` / `TAG_CLOS_ID`）的是非 float 值；
  **不落入该区间的所有位模式都是 float**——有限 double、±Inf、canonical NaN
  一律按位原样存储。`val_is_float` 因此就是一条区间测试：top 16 bits 是否
  在 `[TAG_FIRST, TAG_LAST]` 之外。
- **int**：低 48 位存符号扩展的 int48——实际可用位数是 48，不是 64。
- **float**：全精度 64 位 double，**不堆分配**（早期规划的 `TAG_FLOAT` 指向
  堆上 double 的方案未采用）。NaN 位模式会与标签区冲突，装箱时
  （`val_float` / `val_from_double`）被规范化为 `VAL_CANON_NAN`
  （0x7FF8000000000000，打印即 `nan`），NaN payload 位不保留；-Inf
  （top 16 = 0xFFF0）恰在标签区之下，无需修正。混合运算经 `val_to_double`
  把 int 加宽成 double，凡沾过 float 的结果保持 float——`1.0 + 2` 得
  `3.0`，绝不窄化回 int。
- **string**：堆上 `HeapString { hdr, len, data[] }`。存储按 NUL 终止分配，
  但 `len` 才是权威长度（data 可含内嵌 NUL 字节）——取长度一律用 `len`，
  不要 `strlen`。
- **pair / closure / string / bytes** 是仅有的四种堆对象（`HEAP_PAIR` /
  `HEAP_CLOS` / `HEAP_STRING` / `HEAP_BYTES`），Val 低 48 位是堆内指针
  （`val_as_pair` / `val_as_clos` 取回）；`TAG_CLOS_ID` 是免堆分配的直接
  fn_id 闭包。堆指针会被 GC 移动，见 §3。
- **symbol** 是 VM 符号表的整数索引，`vm_intern_symbol(vm, name)` 可新建/
  取回。符号表是 VM 级共享状态，intern 在 `vm->sym_lock` 互斥锁下进行
  （`api.c`），运行期从 worker 线程调用是安全的；表增长用 malloc+copy 并
  retire 旧数组（不 realloc），已发布的 `vm->symbols` 指针不会悬空——
  C 模块无需在初始化阶段预 intern 符号，运行期动态 intern 没有问题。
  **读**符号名走 `vm_symbol_name(vm, idx)`（同一把锁，越界返回 NULL）：
  `vm->symbols` 与 `vm->sym_count` 是两块独立内存，无锁读会把扩容前的旧
  数组和扩容后的新计数拼在一起，读到已退役数组之外。返回的字符串由 VM
  拥有（`strdup` 后不再移动），解锁后指针仍有效。

## 3. 分配与 GC 心智模型（E2）

GC 是 **Cheney 半区复制**（per-proc）：存活对象被拷到另一半，堆内绝对指针
（`Val`）随之更新。收集**只发生在分配时**：`proc_heap_alloc` 在堆超过
`gc_trigger` 且 `gc_gate == 0`（当前没有不可根化的活引用）时**就地**收集。VM 主
循环里已经没有任何 GC 检查。

**触发时机**：`proc_heap_alloc` 发现 `heap_ptr > gc_trigger` 时，门开着就当场收集，
门关着则只置 `gc_pending`（**请求**），等门重新开为 0 后由 `proc_gc_drain` 补收。
`gc_trigger` 取**上次存活集的 2 倍**（下限 4 KiB；无上限——gc.c 注释明言
「No ceiling」，扩容交给增长式收集），所以存活集
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
2. **arena 只在收集点移动/扩容**：actor 的堆+栈在同一个 `mem` 块里——起始是
   512 B 空转缓冲，首次堆分配切到 512 B 的 `TA_PROC_HEAP0`，之后**经收集点
   翻倍增长**（live set 复制进更大的 semispace），上限 `TA_ACTOR_HEAP`
   （默认 64 MiB）。而 C 回调整段关门（`gc_gate`），期间不发生收集，也就不
   移动、不扩容；搬家只发生在 GC 门内且活引用全在 TA 栈上。所以「指向堆内
   缓冲区的裸指针」（例如解析 HTTP 时 `char *path_start` 指向某个 HeapString
   的 data）在整个回调期间也一直有效。

于是 C 模块可以自由地在多次分配之间持有 `Val`、`char *`——**旧的 `gc_root_push` /
`GC_ROOTS_SCOPE` 机制已删除**（issue #136；门模型与安全点定义见
`design-decisions.md` D11，收集点从「每条 opcode 边界检查」迁到「分配时 + `gc_gate`
门」是 #150）。剩下两条沿用不变：

1. **构造即返回的对象安全**：`val_string`/`val_int`/`val_pair` 构造完就返回的 Val
   永远不需要保护——构造函数内部先分配、后写值。
2. **模块自己 malloc 的东西 GC 不管**：文件描述符、`FILE*`、socket、大缓冲区
   用 int/指针包装，生命周期归模块管（`lib/net.c` 的 fd 就是 int 返回）。

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
峰值 arena 需求 32 KiB（空转 actor 只占 512 B 空转缓冲，首次分配才切到
512 B 的 `TA_PROC_HEAP0`，此后经收集点翻倍增长）。

## 4. 错误约定：C 信号词汇表

C 原语层只出**原子信号**，TA 包装层负责 lift 成类型化 API（Result/Option）。
全库统一按 `lib/net.c` 的真实 ABI（`net_read`/`net_write`/`net_connect`，
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

## 5. C 模块的状态：数据还是资源

**模块不得有全局可变状态**（全局表、全局句柄池、全局缓存、全局 mutex）——
这跟 actor 并发模型相悖：actor 的状态属于 actor，跨 actor 只用 mailbox 传值；
模块级全局表把「这份状态归谁」变成「谁来加锁」。

- **状态是数据 → 做成值**（首选）。C 侧写成纯函数：输入状态，输出下一个状态，
  状态由 actor 一路传下去（genserver 传 State 的样子）。`lib/sha256.c` 是范例：
  状态 = 摘要的中间状态（一段字节），C 侧是 `raw_update(state, data) -> state`，
  模块里没有表、没有句柄、没有锁。复制一个 hasher 给别的 actor = 复制状态
  （接收方接着 sender 的摘要算），没有共享 context 要仲裁，也就没有二次释放。
- **状态是资源 → 先问「谁拥有它」，再问「怎么共享」**。Erlang/OTP 的排序是
  「值 → 单 owner + 消息 → 共享」三档，TA 照抄（详见
  [AGENTS.md「C 模块不得有全局状态」](../AGENTS.md#c-模块不得有全局状态)）：

  1. **值化**（同上一档）。
  2. **单 owner actor + mailbox**（Erlang 的 port / gen_server 做法）：资源归
     一个 actor 所有，其它 actor 只发消息请求 owner 代做；C 侧记录 owner 的
     pid，非 owner 调用直接报错。访问因此天然串行，不需要锁。
  3. **真共享**：锁按资源分（每个资源一把、随资源生灭），不是全局一把；锁只
     护资源自身的不变量，不跨昂贵操作（不跨 syscall / IO / 可能触发 GC 的分配）。
  4. **`Resource` 不透明值**（#401 落地）：可变 C 资源的标准形态。C 侧
     `ta_resource_new(ptr, dtor)` 造出堆外 payload（共享 `released` 标志所在）
     + 每持有者一份堆内 header；值随 send/spawn 移交归属，GC / actor 死亡 /
     VM teardown 三重兜底扫描——忘记 close 也不泄漏。`lib/buffer.c` /
     `lib/process.c` / `lib/tls.c` 就是这么做的。

- **语言层缺口已随 #401 补上**：Erlang `enif_alloc_resource` 式的 opaque
  resource term 就是上面第 4 档的 `Resource`——不透明值、随 send/spawn 移交、
  GC 释放不可达的最后一份 header，没有槽表、没有 ABA、没有二次释放，也不怕
  actor 死亡泄漏。设计与四条释放路径见 [c-resource-design.md](c-resource-design.md)。

  可变 C 资源（buffer / process / tls / sqlite / sdl 等）按 `Resource` 值包装：

- **类型**：单构造 ADT 包一层——`type Buffer { Buf(Resource) }`（`lib/process.ta`
  的 `P(Resource)`、`lib/tls.ta` 的 `Tls(Resource)` 同形），纯语言现有特性；
  `Resource` 本身不透明：无构造子、不可 `match`、不可比较（相等性按 header
  指针恒等）、打印 `<resource>`、没有 `resource?` 谓词。
- **跨边界 ABI**：TA 层 match 解包后，**C 收 `Resource`**（`ta_resource_*` API）。
  **防伪造交给 typecheck**：ADT 构造器即能力证明；C 侧资源分支开头做一次
  owner 检查（`r->owner_pid != 当前 pid` → 报错）。
- **持有资源的值绝不能是可拷贝的裸值**（int 句柄、塞指针的 Bytes/String）：
  深拷贝会让收发双方各持一个「句柄」指向同一资源，两边都 close 就是二次释放
  （堆损坏；ASan 报 "attempting double-free"）。`Resource` 每持有者一份堆内
  header、堆外共享 payload，`released` 在 payload 上——副本间天然幂等，
  send/spawn 移交归属。
- **失效句柄**：已 close 再操作 → C 返回硬错误，TA 层 lift 成 `Err("closed")`；
  **double close 幂等**（payload `released` 拦住第二次 dtor）。
- **生命周期**：显式 `close` 最优先；不 close 也不泄漏——四条释放路径兜底：
  `close()` / GC fromspace 扫描 / actor 死亡（`proc_die` 活堆 + 邮箱 fragment
  排空）/ VM teardown（`vm_free`，含 CLI 正常退出收尾），见设计 §6。
- **dtor 约束**：dtor 在 GC 尾声、`proc_die`、`vm_free` 里跑——不得分配 TA
  值、不得重入 VM、不得再触发收集；可以阻塞（`lib/process.c` 的 dtor 是
  `kill`/`waitpid`，跑在 GC 尾声里，明说可接受）；`ta_resource_new` 不得在
  cfunc 回调窗口内调用（三段非阻塞 cfunc 协议，见设计 §6.1）。
- **语义标注**：buffer / process / tls / sqlite / sdl = **可变资源**（C 侧原地
  改）；str / list / dict / sha256 = 值。可变资源是对 TA 层函数式的明确例外，
  confined 在 C 边界后。

## 6. 注册与加载

| 方式 | 适用 | 做法 |
|------|------|------|
| 静态模块 | 核心运行时 | `void vm_register_<mod>_module(VM *vm)` 在 `src/main.c` main 调用；函数表里的名字自动注册成 `"<mod>.<func>"` |
| 动态模块 | 第三方/试验 | 编译成 `lib/<mod>.dylib`；装载 .tabc 时（link_unit）`dlopen` + `dlsym("vm_load_self")`（main.c 实现，参考 `lib/demo.c`） |

动态模块编译（参考 Makefile 的 demo 规则）：

```sh
cc -shared -fPIC -I. -o lib/mymod.dylib lib/mymod.c
```

（sanitizer 构建加 `-DTA_MOD_TAG=asan`，产物 `lib/mymod_asan.dylib`，
tavm 自动选与自己匹配的 tag。）

### 6.1 模块 ABI 校验（#264 E3）

`ta.h` 同时是一份 **ABI 契约**：模块直接操纵 NaN-boxing 的 `Val`、`VM` 结构体与
opcode 集，VM 一旦改动这些表示，旧 dylib 会**静默写坏内存**。因此 `ta.h` 自带一个
版本探针——**`#include "ta.h"` 即自动导出**，无需手写：

```c
#define TA_ABI_VERSION 1                          // ta.h
__attribute__((weak)) int ta_abi_version(void) { return TA_ABI_VERSION; }
```

装载时 `find_cfunc_autoload`（`src/main.c`）`dlsym` 读回探针并与 VM 的
`TA_ABI_VERSION` 比对：**不一致，或旧到没有探针，就当场拒绝加载**，报出模块名、
文件与双方版本——而不是放行后跑出乱码。你**什么都不用做**；只有当 VM 维护者
bump 了 `TA_ABI_VERSION`（`Val` 布局 / opcode 集 / `ta.h` 结构体变动）时，重建
模块即可。`weak` 联结让同一个 `ta.h` 能被宿主多个 TU 与任意多个模块同时包含
而不冲突。

## 7. 类型注册：外部 fn 声明文件（P4 治理）

**现状**：C 模块函数的类型签名由 **外部 fn 声明文件** 提供——每个 C 模块在
`lib/` 下配一个 `<mod>.ta`，一行一个签名：

```ta
// lib/demo.ta — demo C 模块的类型声明
external fn demo.double(Int) -> Int
external fn demo.greet(String) -> String
external fn demo.pair(Int, Int) -> Pair
```

参数也可以写成命名形式（`name : Type`），语义与位置形式完全相同 —— 参数名是
给读代码的人看的，只有类型进类型环境：

```ta
external fn demo.pair(a : Int, b : Int) -> Pair    // ≡ demo.pair(Int, Int)
external fn buffer.raw_push_str(h, s) -> Int       // 裸名字 = 任意类型
```



编译期 `import <mod>` 时 driver 加载该声明文件，typechecker 把 `external fn`
签名注册进类型环境（Pass 0.5 的 `extend_env_from_sigs`），模块调用按声明严格
类型检查；不 `import` 则走宽容的 dotted-call 路径（可调用但无类型承诺）。
**新增 C 模块不需要碰 `lib/bootstrap/typecheck.ta`**——签名走 `lib/<mod>.ta`
声明文件而非硬编码。注意存量例外：`str.eq`/`str.concat`/`str.length` 等 `str.*`
签名至今仍硬编码在 `make_builtin_env`（typecheck.ta:3063 起），与 `lib/str.ta` 的
external 声明**双源并存**；新模块一律走声明文件，不要模仿这个存量双源。

**规则**：
- 加/改 C 模块函数 → 同步更新 `lib/<mod>.ta` 声明文件（签名即文档）
- 声明文件与 C 侧 `TaFunc` 表要保持一致，否则会重现幻影 builtin（typecheck
    承诺、runtime 没有）。声明文件机制确保类型签名有单一权威来源。

## 8. 参考实现

- `src/net.c`（最小、无 GC root、非阻塞 IO + vm_yield 挂起）——**首选模板**
- `lib/tls.c`（lazy dylib + os.c 式 dotted 注册 + `Tls(Resource)` 资源值胶水
  （dtor 释放 SSL/BIO）；
  net 信号协议在 WANT_READ/WANT_WRITE 上的复用）
- `lib/str.c`（string 模块，含分配）
- 注册/加载机制：静态注册 `src/api.c` 的 `vm_register_module`；动态装载在
  `src/main.c`（dlopen + `dlsym("vm_load_self")`，无 `vm_load_c_module` 这个函数）
