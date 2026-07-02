# 泛型 ADT 设计 (Route C) — 修订版

## 核心原则

**VM 原语（cons/nil/car/cdr）是实现细节，不对用户暴露。**
用户通过 ADT 构造器和模式匹配操作数据。
类型签名在构造器上，不在 VM 原语上。

## 类型表示变更

### 新增：`('app Name args)`

```
('app List ((tvar 0)))           → List('a)
('app List ((base int)))         → List(int)
('app Result ((base int) (base string)))  → Result(int, string)
```

### 需要改动的 typecheck 函数

| 函数 | 改什么 |
|------|--------|
| `t_app(name, args)` | 新增构造器 |
| `apply_subst` | 递归进 args |
| `occur_check` | 递归进 args |
| `unify_resolved` | `app` vs `app`：name 相同 → 逐个 unify args；name 不同 → fail；`app` vs `tvar` → 同 tvar 逻辑 |
| `type_format_resolved` | 打印 `Name(arg1, arg2)` |
| `collect_variants` | 支持 type params |
| `parse_type_annot` | 支持复合类型 `List(int)` |
| `free_vars_t` | 递归进 app args |

## ADT 语法

```ta
// 泛型
type List(a) { Nil; Cons(a, List(a)) }
type Result(a, e) { Ok(a); Error(e) }
type Option(a) { None; Some(a) }

// 无参数（仍然支持）
type Color { Red; Green; Blue }
```

### parser AST 格式

```ta
type List(a) { Nil; Cons(a, List(a)) }
→ (type List (a) (quote Nil) (Cons a (List a)))

type Color { Red; Green; Blue }
→ (type Color () (quote Red) (quote Green) (quote Blue))
```

格式：`(type Name (type_params...) variants...)`

## collect_variants 行为

```ta
type List(a) { Nil; Cons(a, List(a)) }
```

1. type_params = `['a]`，分配 tvars：`a → tvar(0)`
2. 结果类型 = `t_app('List, [tvar(0)])` = `('app List ((tvar 0)))`
3. `Nil`（nullary）→ `forall(0). List('a)`
4. `Cons`（n-ary，字段 `a` 和 `List(a)`）：
   - 字段类型解析：`a` → `tvar(0)`（查 param_map），`List(a)` → `t_app('List, [tvar(0)])`
   - 箭头类型：`tvar(0) -> t_app(List, [tvar(0)]) -> t_app(List, [tvar(0)])`
   - scheme：`forall(0). 'a -> List('a) -> List('a)`

### 字段类型解析规则

```
resolve_field(sym, param_map, type_env):
  if sym in param_map → param_map[sym]  (类型变量)
  else → t_base(sym)  (引用其他类型，暂不解析参数)
```

后续可以扩展为递归解析 `List(a)` 这样的嵌套引用。

## List 的处理

**用户层面**：
- `type List(a) { Nil; Cons(a, List(a)) }` 作为标准库或内置类型
- 构造：`Nil`、`Cons(1, Cons(2, Nil))`
- 解构：模式匹配 `Cons(head, tail) -> ...`

**codegen 映射**（无需改 VM）：
- `Nil` → `nil`（运行时就是 nil）
- `Cons(x, xs)` → `cons(x, xs)`（运行时就是 pair）
- 模式匹配 `Cons(head, tail)` 已经通过 cons 解构实现

**typecheck 内部**：
- `make_builtin_env` 中不再给 `cons`/`nil`/`car`/`cdr` 赋用户类型
- 这些是编译器内部使用的原语，typecheck 自己的代码用到它们时不需要精确类型
- 可以选择：从 builtin env 移除，或保留宽松多态类型供内部使用

## 类型注解

### 当前

```ta
fn add(x: int, y: int) -> int { x + y }
```

### 目标

```ta
fn length(lst: List(int)) -> int { ... }
fn safe_div(a: int, b: int) -> Result(int, string) { ... }
```

### parser 变更

注解解析支持复合类型：
- 简单：`int`、`string`、`Color`
- 复合：`List(int)`、`Result(int, string)`

`parse_type_annot` 递归化：遇到 `Name(args)` 构造 `t_app`。

## 实施计划

### Phase 1: typecheck 核心扩展
- `t_app` 构造器
- `apply_subst`、`occur_check`、`free_vars_t` 支持 app
- `unify_resolved` 支持 app vs app / app vs tvar
- `type_format_resolved` 支持 app 打印

### Phase 2: collect_variants 泛型支持
- 解析 type_params
- param_map 映射
- 字段类型解析
- 构造正确的 forall scheme

### Phase 3: parser 语法扩展
- `type List(a) { ... }` 解析
- 类型注解复合类型解析

### Phase 4: 验证
- 测试泛型 ADT 推导
- 测试 List 类型推导
- 确保 bootstrap 不受影响

## 不改动

- VM（无新操作码）
- codegen（类型不影响代码生成）
- tokenizer（现有 token 足够）