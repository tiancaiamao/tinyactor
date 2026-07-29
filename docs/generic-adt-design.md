# 泛型 ADT 设计 — 实现记录

## 状态：✅ 已实现

泛型 ADT 已在 typecheck.ta 中完整实现，并通过 `test/scripts/adt-basic.ta` 等测试验证。

## 核心原则

**VM 原语（cons/nil/car/cdr）是实现细节，不对用户暴露。**
用户通过 ADT 构造器和模式匹配操作数据。
类型签名在构造器上，不在 VM 原语上。

## 类型表示

### `('app Name args)`

```
('app List ((tvar 0)))           → List('a)
('app List ((base int)))         → List(int)
('app Result ((base int) (base string)))  → Result(int, string)
```

## ADT 语法

```ta
// 泛型
type List(a) { Nil; Cons(a, List(a)) }
type Result(a, e) { Ok(a); Error(e) }
type Option(a) { None; Some(a) }

// 无参数
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

## 实现细节

### typecheck 中改动的函数

| 函数 | 改动 |
|------|------|
| `t_app(name, args)` | 新增构造器 |
| `apply_subst` | 递归进 args |
| `occur_check` | 递归进 args |
| `unify_resolved` | `app` vs `app`：name 相同 → 逐个 unify args；name 不同 → fail；`app` vs `tvar` → 同 tvar 逻辑 |
| `type_format_resolved` | 打印 `Name(arg1, arg2)` |
| `collect_variants` | 支持 type params |
| `parse_type_annot` | 支持复合类型 `List(int)` |
| `free_vars_t` | 递归进 app args |

### codegen 映射（无需改 VM）

- `Nil` → `nil`（运行时就是 nil）
- `Cons(x, xs)` → `cons(x, xs)`（运行时就是 pair）
- 模式匹配 `Cons(head, tail)` 已经通过 cons 解构实现

### 不改动

- VM（无新操作码）
- codegen（类型不影响代码生成）
- tokenizer（现有 token 足够）

## 验证

| 测试 | 验证内容 |
|------|----------|
| `test/scripts/adt-basic.ta` | 基础 ADT：定义 `Color{Red;Green;Blue}` + match |
| `test/scripts/module-adt.ta` | 模块间 ADT 传递：`import msg` + `Ping/Pong/Stop` |
| `test/scripts/exhaustiveness.ta` | 穷尽性检查 warning |
| `test/scripts/type-pass.ta` | 类型检查通过场景 |
| `test/scripts/typecheck-clean.ta` | 干净的类型检查 |
| `test/scripts/typecheck-errors.ta` | 类型错误报告 |

## 已知限制

- `nil` 的类型问题：HM 无法区分空列表和空值，`nil` 推导为 fresh tvar
- 箭头类型注解暂不支持：不能写 `(int -> int)` 作为参数注解
- 类型错误为宽容模式：不阻塞编译，通过 `--check` 标志报告