# 泛型 ADT 实现 Spec

## 目标

让 TA 类型系统支持参数化类型 `List(a)`、`Result(a, e)` 等。

## 核心原则

VM 原语（cons/nil/car/cdr）是实现细节，不对用户暴露类型签名。
用户通过 ADT 构造器操作数据。类型签名在构造器上。

## 验收标准

### A1: 类型表示 `t_app`
- `typecheck.ta` 新增 `t_app(name, args)` 构造器
- `apply_subst` 递归进 app args
- `occur_check` 递归进 app args
- `free_vars_t` 递归进 app args
- `unify_resolved`: app vs app（name 相同→逐个 unify args，name 不同→fail）、app vs tvar（同 base 逻辑）、app vs base（fail）、base vs app（fail）
- `type_format_resolved` 打印 `Name(arg1, arg2)` 格式

Verify: 写测试 TA 程序，手动构造 `t_app` 类型并 unify，打印结果。

### A2: 泛型 ADT collect_variants
- `collect_variants` 支持 `type List(a) { Nil; Cons(a, List(a)) }`
- type_params 分配 tvars，结果类型为 `t_app(name, [tvars])`
- nullary 变体类型为 `forall(ids). App(Name, [tvars])`
- n-ary 变体字段类型解析：类型参数→tvar，其他→t_base 或 t_app（递归）
- 无参数类型 `type Color { Red; Green; Blue }` 仍然工作（结果类型为 t_base，向后兼容）

Verify: `type List(a) { Nil; Cons(a, List(a)) }` → Nil 类型 `forall(a). List(a)`，Cons 类型 `forall(a). a -> List(a) -> List(a)`。

### A3: parser 解析泛型 ADT
- `type List(a) { ... }` 解析，type_params 存入 AST
- AST 格式：`(type Name (params...) variants...)`
- 无参数时 params 为空 list `()`

Verify: parser 能解析 `type List(a) { Nil; Cons(a, List(a)) }` 和 `type Color { Red; Green; Blue }`。

### A4: 类型注解支持复合类型
- `parse_type_annot` 递归化，支持 `List(int)`、`Result(int, string)`
- parser 注解解析：`fn f(lst: List(int)) -> int { }` 产生正确的 type-sig

Verify: 注解 `List(int)` 能被解析为 `t_app('List, [t_int()])`。

### A5: 端到端推导
- 用户写 `type List(a) { Nil; Cons(a, List(a)) }` 然后 `fn single(x) { Cons(x, Nil) }` 推导出 `forall(a). a -> List(a)`
- Bootstrap 不受影响（`make && make bootstrap` 通过）

Verify: 完整测试程序，检查推导结果。

## 约束

- 不改 VM（无新操作码）
- 不改 codegen
- 不改 tokenizer
- 向后兼容：无参数 ADT 仍然工作
- typecheck 仍是宽容模式（推导失败不阻塞编译）