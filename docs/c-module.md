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
        return val_nil();                  // 错误约定：失败返回 nil
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

TA 侧用法（类型签名在 `lib/<mod>.ta` 外部 fn 声明文件，见 §6）——调用
**不需要 `import`**（首次调用运行时懒加载 dylib）；要编译期严格类型检查则
`import <mod>` 加载声明文件：

```ta
print(mymod.double(21))   // 42
print(mymod.double("x"))  // nil（错误约定）
```

首次调用 `mymod.double` 时运行时自动 `dlopen lib/mymod.dylib` 并注册（编译期
codegen 检测到 `lib/mymod.dylib` 存在即生成懒加载调用；TA 模块无 dylib，
走普通模块解析，互不干扰）。`import mymod` 会加载 `lib/mymod.ta` 声明文件
（若存在），从而让模块调用通过编译期类型检查（见 §6）；无声明文件时
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
（`Val`）随之更新。但 **GC 只在 opcode 边界发生**——`vm_run_proc` 取指令前检查
`gc_pending`，分配本身只是**请求** GC（置位），从不就地收集。

**触发时机**：`proc_heap_alloc` 发现 `heap_ptr > gc_trigger` 时置 `gc_pending`，
由下一个 opcode 边界消费。`gc_trigger` 取**上次存活集的 2 倍**（下限 4 KiB，上限
arena 的 3/4），所以存活集越大、收集越稀。没有后台 GC 线程，也不会被别的进程
触发（每个 Proc 独立 arena，GC 只收自己的）。

**因此 C 模块不需要任何 root**。以前要 `gc_root_push` / `GC_ROOTS_SCOPE` 的两条
理由，现在都被结构性保证了：

1. **一条 opcode handler 之内不会发生 GC**：`TaFunc` 回调总是在某条指令内部被
   调用，期间 GC 最多被「请求」，要等回调返回、VM 走到下一条指令才可能真正执行。
   所以回调里的 C 局部 `Val` 在整个回调期间都指向同一个活对象。
2. **arena 一旦持有对象就不再移动/扩容**：actor 的堆+栈是**固定预留**
   （`TA_ACTOR_HEAP`，默认 64 MiB），首次分配时一次性预留，之后终生不 realloc。
   预留只允许发生在 `heap_ptr == 0`（还没有任何对象）时——空 arena 里不存在指向
   它的指针，搬家对谁都不可见。所以「指向堆内缓冲区的裸指针」（例如解析 HTTP 时
   `char *path_start` 指向某个 HeapString 的 data）在整个回调期间也一直有效。

于是 C 模块可以自由地在多次分配之间持有 `Val`、`char *`——**旧的 `gc_root_push` /
`GC_ROOTS_SCOPE` 机制已删除**（issue #136；设计取舍见 `design-decisions.md` D11）。
剩下两条沿用不变：

1. **构造即返回的对象安全**：`val_string`/`val_int`/`val_pair` 构造完就返回的 Val
   永远不需要保护——构造函数内部先分配、后写值。
2. **模块自己 malloc 的东西 GC 不管**：文件描述符、`FILE*`、socket、大缓冲区
   用 int/指针包装，生命周期归模块管（`src/net.c` 的 fd 就是 int 返回）。

唯一的例外形状：**把 `Val` 存进模块自己的静态变量、跨回调使用不成立**。这类值
不在 GC 扫描的位置，半区复制后即失效。需要跨回调/跨进程长期持有的数据请走消息
（`MsgFragment`），不要自己缓存 `Val`。

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

## 4. 错误约定

**失败统一返回 `nil`**。TA 侧用 `if null?(x)` 检查。

- 合法值类型检查失败（参数不是预期类型）→ `val_nil()`
- 运行期失败（越界、打不开、连不上）→ `val_nil()`
- 例外：net 模块沿用历史 `-1`（fd 失败哨兵）、str.char_at 沿用 `-1`——
  **历史包袱不迁移**；新模块一律 nil。

## 5. 注册与加载

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

## 6. 类型注册：外部 fn 声明文件（P4 治理）

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

## 7. 参考实现

- `src/net.c`（最小、无 GC root、非阻塞 IO + vm_yield 挂起）——**首选模板**
- `src/str.c`（string 模块，含分配）
- `src/http.c`（动态 dylib 模块，最复杂）
- 注册/加载机制：`src/api.c`（vm_register_module / vm_load_c_module）