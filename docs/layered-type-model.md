# 分层类型模型（Layered Type Model）

> 设计原则：**类型检查只发生在上层语言；下层实现是无类型的。**
> 但底层原语**不是完全不承诺类型**——按语义通用性取光谱。
>
> 本文档对应 `docs/improvement-plan.md` 阶段 C4，是对 B3/C3 落地行为的正式描述。

---

## 1. 两层模型

```
┌─────────────────────────────────────────────────┐
│  上层 typed surface（TA 源码）                    │
│  · ADT 构造器 + 模式匹配                         │
│  · 带签名 fn（a -> b 注解、泛型、HOF）            │
│  · 类型化封装函数（"kons" 类，见 §5）             │
├─────────────────────────────────────────────────┤
│  下层 untyped（VM / C 模块 / 字节码）             │
│  · tavm 运行时（NaN-boxing 值，tag=0xff01 等）    │
│  · C builtin 模块（str / vm / buf / file / net）  │
│  · 跨模块点号调用（module.fn）——模块加载期解析    │
└─────────────────────────────────────────────────┘
```

类型检查器（`lib/typecheck.ta`）只对上层源码做 Hindley-Milner 推断。
下层要么被上层显式承诺类型（§3），要么在加载期/运行时自行解析（§4），
类型检查器对后者保持宽容。

---

## 2. 光谱规则（逐原语类型承诺）

按语义通用性，原语分成两类：

| 类别 | 判定标准 | 类型承诺 | 示例 |
|------|---------|---------|------|
| **真·通用原语** | 对任意值都成立，语义与具体类型无关 | 宽松多态（forall） | `cons` / `car` / `cdr` / `print` / `null?` / `pair?` / `int?` / `string?` / `symbol?` / `len` / `list_ref` / `spawn` / `send` / `recv` / `monitor` / `receive-scan` |
| **语义绑定原语** | 语义强绑定到具体类型，传错类型必然是 bug | 具体类型（无 forall） | `str.*` 全部 / `vm.get_arg` / `+ - * /` / `< > <= >=` / `not` / `self` |

**示例**（C3 落地）：
- `str.to_int('Red)` → 拒绝（symbol ≠ string）✓
- `str.to_sym(42)` → 拒绝（int ≠ string）✓（C3 之前能通过，是过度放宽）
- `str.sym_to_str(42)` → 拒绝（int ≠ symbol）✓（C3 之前能通过）
- `cons(1, "a")` → 通过（heterogeneous pair 合法，元素类型不受约束）
- `print(anything)` → 通过

---

## 3. 内置类型承诺清单（C3 审计产出）

来源：`lib/typecheck.ta` 的 `make_builtin_env`。分类：**宽松** = forall 多态，**绑定** = 具体类型。

### 宽松（真·通用）

| 原语 | 承诺类型 | 备注 |
|------|---------|------|
| `car` / `cdr` | `forall(a b, a -> b)` / `forall(a, a -> a)` | heterogeneous pair 合法 |
| `cons` | `forall(a b, a -> b -> b)` | 元素类型不受约束 |
| `print` | `forall(a c, a -> c)` | 返回类型自由 |
| `null?` `pair?` `int?` `string?` `symbol?` | `forall(a, a -> bool)` | 类型谓词 |
| `len` | `forall(a, a -> int)` | |
| `list_ref` | `forall(a, a -> int -> a)` | |
| `spawn` | `forall(a, a -> pid)` | 任意消息初始值 |
| `send` | `forall(a b, a -> b -> b)` | 任意消息 |
| `recv` / `recv-commit` / `recv-skip` | `forall(a, a)` | 消息类型自由 |
| `monitor` | `forall(a b, a -> b)` | |
| `receive-scan` | `forall(a b, (a -> b) -> b)` | |
| `vm.parse_source` | `forall(a, string -> a)` | 解析产物类型自由 |
| `vm.is_builtin_module` | `forall(a, a -> bool)` | |
| `vm.load_bytecode` | `forall(a, a -> int)` | |

### 绑定（语义绑定）

| 原语 | 承诺类型 | 备注 |
|------|---------|------|
| `str.eq` | `string -> string -> bool` | |
| `str.concat` | `string -> string -> string` | |
| `str.length` | `string -> int` | |
| `str.char_at` | `string -> int -> int` | |
| `str.substr` | `string -> int -> int -> string` | |
| `str.to_int` | `string -> int` | |
| `str.from_int` | `int -> string` | |
| `str.index_of` | `string -> string -> int` | |
| `str.to_sym` | `string -> symbol` | **C3 收紧**（原 `forall(a, string -> a)`） |
| `str.sym_to_str` | `symbol -> string` | **C3 收紧**（原 `forall(a, a -> string)`） |
| `vm.get_arg` | `int -> string` | |
| `self` | `pid` | 无参 |
| `+ - * /` | `int -> int -> int` | |
| `< > <= >=` | `int -> int -> bool` | |
| `not` | `bool -> bool` | |

---

## 4. 下层的宽容面（B3 落地行为）

### 4.1 跨模块点号调用（module.fn）

`typecheck.count_errors`、`codegen.compile` 这类**跨模块函数不在当前 env**——
它们由模块加载期解析（`import` 时注册）。B3 的 undefined 检测对
**带点符号（`str.index_of(sym, ".") > 0`）豁免**，保持历史宽松行为：

- `typecheck.count_errors(...)` → 通过（加载期解析）
- `foo(...)`（无点、非 let 绑定、非内置）→ `[E0002] undefined function 'foo'`

### 4.2 语言关键字：match / receive

`match` 和 `receive` 的**模式位置**（构造器、`_` 通配、绑定变量）是 codegen
的职责，**不参与类型推断**（B3 前会误入函数调用路径报 E0002）：
- 只推断 scrutinee + 每个 arm 的 body（宽松）
- 模式中的构造器/变量不会被 typecheck

### 4.3 块级 let（braced block 语句）

`{ let f1 = make_closure(20)  let f2 = f1(10) ... }` 中，`let` 是无 body
语句，绑定延伸到**同块后续语句**。typechecker 的 let 形式假设 Lisp 嵌套语义，
因此**预注册块内所有 let 绑定名**（fresh tvar，宽松）：
- `f1`/`f2` 不会误报 undefined
- 保持类型宽松（与运行时作用域一致）
- 真·未定义（未 let 绑定、未定义 fn）仍报 `[E0002]`

### 4.4 C 模块（buf / file / net / bytes）

这些模块**不在 typecheck env**（tavm 加载期注册）。调用是宽松的（点号豁免），
运行时负责错误处理。`bytes` 模块未实现——`bytes(...)`/`bytes-length(...)`
编译被 typecheck 拒绝（`[E0002]`，见 `test/basic/bytes-basic-errors.ta`）。

---

## 5. "kons" 类封装的定位

当下层行为通用但类型信息丢失时，在上层用**带签名函数封装**恢复类型安全：

```ta
// 例：给宽松的 cons 语义一个强类型的 List 门面
fn kons(x: int, xs: List(int)) -> List(int) {
  Cons(x, xs)          // 走 ADT 构造器，类型安全
}
```

- 封装函数必须带完整签名（`a -> b` 注解），享受 HOF 泛型（C1/C2）
- 定位：**上层安全面的第一道防线**；是否引入按需求决定，不是必须项
- 反例（禁止）：把绑定原语包进宽松封装（如 `fn id(x) { x }` 再传 `str.to_int`）

---

## 6. 错误码

| 码 | 含义 | 示例输出 |
|----|------|---------|
| `E0001` | 类型不匹配 | `[E0001] in function 'main' (line 5): arg 2 of map: cannot unify string with List(int)` |
| `E0002` | 未定义函数/符号 | `[E0002] in function 'main' (line 1): undefined function 'foo'` |

结构化错误（`(kind . (ctx . payload))`）为未来 LSP 铺路（阶段 B 收尾）。

---

## 7. 验证

- `make test`：92/92 全绿（含 HOF 注解、match/pattern、bytes-basic-errors 负例）
- 自举 fixed point：`make bootstrap` 产物 md5 稳定
- C3 负例：`str.to_sym(42)` / `str.sym_to_str(42)` 均报 `E0001`
- B3 负例：未定义函数报 `E0002`；match/`_`/跨模块点号调用不误报