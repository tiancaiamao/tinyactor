# cons 退场设计：把 form 层的「点对」换成有名字的类型

> 状态：**设计（待评审）**。实测数字由 `scripts/cons-probe.sh` 复现（方法见 §2）。
> 关联：docs/kl-format.md §7 偏差 C6（`.kl` 已先一步退役点对）、PR #476、issue #65/#236。

## 1. 目标

语言层只保留一种列表：`List a`。今天 `cons : forall(a b, a -> b -> b)`
（`lib/bootstrap/typecheck.ta` `make_builtin_env`）允许把任意值塞进 cdr，于是

```ta
cons(tok, cons(pos, nil))       // 四字段「手工结构体」
cons(label, slot)               // 关联对
cons(1, expr)                   // 带标记的二元组
```

在编译器和标准库里遍地都是。这些不是列表，是**没有名字的结构体**：读的人要
数 `car(cdr(cdr(x)))`，写的人永远不知道多塞一个字段会不会打坏别人的遍历。
目标是让这类写法**写不出来**，逼出

```ta
type Step { Step(value : Value, next : Int, line_delta : Int) }
type Label { Label(name : Symbol, slot : Int) }
```

即 `cons : a -> List a -> List a`，`car : List a -> a`，`cdr : List a -> List a`。

## 2. 实测：收紧签名会打出 1162 个错误

探针做法：把 `make_builtin_env` 里三个内建改成

```ta
car  : forall(a, List a -> a)
cdr  : forall(a, List a -> List a)
cons : forall(a, a -> List a -> List a)
```
（复现：`scripts/cons-probe.sh`，跑完自动恢复源码与 `lib/bootstrap.tabc`，工作树不留改动。）

然后 `make bootstrap` **两遍**——第一遍用旧种子 `lib/bootstrap.tabc` 编出新编译器
（这一趟必然成功，因为做检查的是老检查器），第二遍用新编译器 self-host，接收
1162 个 `E0001`。

按模块（模块前缀 → 错误站点数）。下表是 PR #476 合入之后的读数；在只含
C6 之前状态的 `main` 上跑同一个探针是 **1174**（多出的 12 个站点在 kl 40 / ast 11 —— 正是 #476 删掉的点对读写路径）：

| 模块 | 行数 | 错误站点 |
|---|---:|---:|
| typecheck | 4497 | 406 |
| parser | 1736 | 302 |
| driver | 2765 | 220 |
| tokenizer | 742 | 163 |
| kl | 627 | 33 |
| covinst | 179 | 24 |
| ast | 296 | 10 |
| bytecode | 197 | 2 |
| modsig | 396 | 2 |
| **合计** | **11435** | **1162**（263 个函数） |

结论：这不是「改两行签名 + 顺手修几十处」，而是**前端表示层的整体改造**。
1162 个站点里 81% 集中在 tokenizer / parser / typecheck 三家的 form 树操作上。

## 3. 为什么不能「模块级」局部修

异构树一旦收敛到**一个类型变量**，递归遍历必然撞上 occur check（`'x = List 'x`）。
实测（covinst，24 个错误里最典型的三条）：

```text
[E0001] in function 'rw': cannot unify List(List(Symbol)) -> String -> Symbol -> List(Symbol) -> List(List(Symbol))
                              with List(Symbol) -> String -> Int -> List(Symbol) -> 'a
[E0001] in function 'rw_list': arg 3 of rw_list: cannot unify List(Symbol) with Symbol
[E0001] in function 'is_marker': arg 1 of car: cannot unify Symbol with List('a)
```

`rw(e, …)` 既被调用在列表位置（`car(xs)` 的元素）又被调用在原子位置，于是
`e : 'x` 同时要求 `'x = List('x)`。**局部修不好**：只要树还是异构的，任何
遍历函数都不可标注、不可检查，`cons` 也只能继续接受任意 cdr。

所以第一步必须是**给树一个类型**，而不是逐个消掉 `cons(a, 任意b)`。

## 4. 两条路线

### 路线 A：加一个通用 `Form` 树类型

```ta
type Form {
  FAtom(c : Const)        // Const 已存在（ast.ta），int/float/string/bool/nil/符号
  FList(items : List(Form))
}
```

`car`/`cdr`/`cons` 的调用点改成 `match`，树本身仍是「原子 + 列表」。
改动机械、可批量、风险低；**代价**是 `car(cdr(cdr(x)))` 这种取字段的写法原样
留在代码里——只是把「类型错误」换成了「运行期焦虑」，读起来仍然要数字段。

### 路线 B（**推荐**）：按管线阶段删掉中间层，每层只留自己类型的表示

不改一个通用树，而是让**每一层的输出就是下一层的输入类型**，中间层直接消失：

```text
.kl 文本 ─kl.read_program→ List(Top)           （删掉 form 中转 + ast.parse_program）
.ta 文本 ─tokenizer→ List(Token) ─parser→ List(TAStmt) ─lower→ List(Top)
```

- `.kl` 不再是「任意树的序列化格式」：程序文本走 `Top`，模块签名文本由 modsig
  自己的条目 ADT 写出（`(label slot)` 那类形状不再需要 `kl.write_flat` 帮忙猜）
- typecheck 吃的是 `TAStmt`/`TAExpr` ADT，`match` 穷尽性检查替掉
  `car(expr) == 'if` 的符号分派，「不可能状态」从类型上就不存在
- 净代码量**下降**：`car(cdr(cdr(x)))` 链换成具名字段，`is_*` 形状判据消失

代价：parser + typecheck（6233 行）是深改，不是机械改。收益是这两个模块从此
可读、可查、可改——这正是 AGENTS.md 说的「移除让限制显得不可或缺的缺陷」。

## 5. 分阶段落地（每步都是独立 PR，且**都保持 fixed point + 全绿**）

| 阶段 | 内容 | 涉及的错误站点 | 验收 |
|---|---|---:|---|
| M0 | 本设计 + 探针脚本（可重复测量） | 0 | 复现 1162 |
| M1 | `.kl` 路径类型化：`kl.read_program → List(Top)`、writer 吃 `Top`；删 `ast.parse_program`/`encode_program`；签名文本改由 modsig 自己写 | kl 33 + ast 10（含 9 个已消） | kl-roundtrip G-A/G-B 全绿；`--emit-kl` 文本**逐字节不变** |
| M2 | tokenizer 产出 `Token` ADT（`kind/payload/line`），parser 吃它 | tokenizer 163 | 词法测试 + parser-ast 全绿 |
| M3 | parser 产出 TA 源 AST ADT，lower-ast 吃它 | parser 302 + driver 里解析相关 | `parser-ast.ta` 全绿 |
| M4 | typecheck 吃 AST ADT（`match` 替掉头部符号分派） | typecheck 406 + driver 剩余 | 全测试 + 类型错误回归全绿 |
| M5 | 收紧三行内建签名；删 `pair?`、spec 的 `Pair` 行、两个点对正例；`cons : a -> List a -> List a` 落地 | 剩余全部 | 探针错误数 1162 → **0**；fixed point + `make test` |

## 6. 过渡策略（重要）

**签名最后才收紧（M5）**。M1–M4 期间 `cons` 仍是宽松的，所以每一步都能编译、
能跑测试、能合并；进度用同一个探针量（错误数从 1162 往下掉），不需要等最后
一刀才知道做没做对。M5 那一刀的风险由 M1–M4 提前消化：剩下多少错误，就还剩
多少迁移没做。

## 7. 风险与已知坑

- **`.kl` 文本不变性**：`--emit-kl` 产物与模块签名文本都被下游算哈希
  （TASIG5）。M1 若改了任何字节，必须 bump 版本并说明（PR #476 已有先例）。
- **C 侧边界**：`lib/sexp.c` 的 `sexp.parse` 返回未类型化树，`kl-roundtrip`
  用它交叉验证 kl reader。M1 之后这条交叉验证需要一次显式的
  「未类型化 → `Top`」桥（或退役该对照，只留 G-A/G-B 幂等）。
- **annotated 函数不泛化**：带具体标注的 TA 函数签名不做泛化（ast.ta §parse 的
  注释记着这条），M1–M4 的中间函数尽量不标注，等类型稳定后再标。
- **不要为了通过检查而引入 `Form` 再在 M3 删掉**：路线 B 的意义就是不走这条
  回头路；M1 只在 `.kl` 路径上做（那里 `Top` 已经存在，不需要新类型）。