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

TA 侧用法（类型签名在 `lib/typecheck.ta` 注册，见 §7）——**不需要 `import`**：

```ta
print(mymod.double(21))   // 42
print(mymod.double("x"))  // nil（错误约定）
```

首次调用 `mymod.double` 时运行时自动 `dlopen lib/mymod.dylib` 并注册（编译期
codegen 检测到 `lib/mymod.dylib` 存在即生成懒加载调用；TA 模块无 dylib，
走普通模块解析，互不干扰）。注意 `import mymod` 目前会报
"module not found: mymod.ta"——`import` 只对 TA 模块（`.ta` 源码）和
内建模块（str/net/file/buf/vm/http，tavm.c 预注册）有效；C 模块直接调用即可。
（`import` 支持 C 模块与类型注册绑定，属 C 阶段 backlog。）

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

- int 是 64 位；TA 里没有 float。
- string 是**字节数组**（`char *data` + `int len`，非 NUL 终止语义——用 `len`）。
- symbol 是 VM 符号表的整数索引，`vm_intern_symbol(vm, name)` 可新建/取回。

## 3. 分配与 GC 心智模型（E2）

GC 是 **Cheney 半区复制**（per-proc）：GC 时堆对象被**移动**，所有指向旧位置的
`Val` 指针都会失效——除非它被 **root** 保护。

三条规则（记牢即可）：

1. **构造即返回的对象安全**：`val_string`/`val_int`/`val_pair` 单步分配、构造完
   就返回的 Val 不需要 root（分配内部已把中间值压 root）。绝大多数模块函数
   属于这一类——看 `src/net.c`，它一个 root 都不用。
2. **跨分配持有 Val 要 root**：如果你要先存一个 Val、再做另一次分配/调用、
   再用那个 Val，用 `GC_ROOTS_SCOPE`：
   ```c
   GC_ROOTS_SCOPE(p, rbase) {          // p = tls_current_proc
       gc_root_push(p, saved);          // 之后 GC 会更新它
       Val s2 = val_string(p, "...", 3); // 这次分配可能触发 GC
       Val got = val_get_car(saved);    // saved 仍是有效的
   }                                    // 作用域退出自动恢复
   ```
3. **模块自己 malloc 的东西 GC 不管**：文件描述符、`FILE*`、socket、大缓冲区
   用 int/指针包装，生命周期归模块管（`src/net.c` 的 fd 就是 int 返回）。

**边界**（文档化心智模型）：

| 内存            | 谁管       |
|-----------------|-----------|
| proc heap 里的 Val 对象（string/pair/bytes/clos） | GC（root 保护） |
| 模块 malloc 的缓冲区/句柄（fd、FILE*） | 模块自己（TA 侧当 int/opaque） |
| MsgFragment（actor 邮箱） | 运行时（VM 管，不属 proc heap） |

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

## 6. 类型注册：双处声明 + 校验（E1 治理）

**现状**：模块函数类型签名唯一权威在 `lib/typecheck.ta` 的 `make_builtin_env`
（TA 侧 extend 链）。**C 侧 TaFunc 表是第二处**。两处失同步 = 幻影 builtin
（typecheck 承诺、runtime 没有——D1 教训，已修 len/list_ref）。

**规则**：
- 加/改 C 模块函数 → **必须**同步 `lib/typecheck.ta` 注册签名
- **`make check-modules`**（tools/check-modules.sh）验证：
  typecheck 注册的每个 `'mod.func` 在 C 侧有同名实现 → **无幻影**；
  C 侧有的、typecheck 没注册的 → 提示（可调用但无类型承诺）

```sh
make check-modules   # CI 里跑，防回归
```

## 7. 参考实现

- `src/net.c`（最小、无 GC root、非阻塞 IO + vm_yield 挂起）——**首选模板**
- `src/str.c`（string 模块，含分配）
- `src/http.c`（动态 dylib 模块，最复杂）
- 注册/加载机制：`src/api.c`（vm_register_module / vm_load_c_module）