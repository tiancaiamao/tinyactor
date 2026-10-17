# `.kl` 中间格式（Phase C）

> `.ta → .kl → .tabc`。`.kl` 是**编译管线中间层**：不是新 runtime、不是用户语言、
> 也不是对外/跨版本稳定格式。内容 = `lower_program` 产物的文本序列化。
>
> writer **和** reader 都在 [`lib/bootstrap/kl.ta`](../lib/bootstrap/kl.ta)
> （`write` / `write_flat` / `write_program` / `read` / `read_program`）⇒ **编译路径
> 不 dlopen 任何 C 模块**（C3，见 `test/run_crash_tests.sh` case (i)：抽掉
> `lib/sexp.dylib` 后编译照常成功）。[`lib/sexp.c`](../lib/sexp.c) 的 Grammar v1
> 仍在原处，但消费面只剩前台工具与运行期（`test/basic/sexp-basic.ta`、用户程序
> 自己的 `import sexp`）；编译路径不再调用它。
>
> 两份 reader 在过渡期并存，纪律不是"各自能跑"而是**等价**：
> `test/compiler/kl-roundtrip.ta` 对同一批输入逐条断言"同一棵规范文本 + 同一句
> 错误文本"（含 `line/col`）。
>
> 从 C4 起 `make bootstrap` 会同趟写出本编译器自身闭包的 `.kl` 文本
> [`lib/bootstrap/driver.kl`](../lib/bootstrap/driver.kl)，但 C5 起**它只是
> 本地/测试产物，不入 git**（`.kl` 不是跨版本稳定格式，入库快照只产出合并
> 冲突）。稳定性改由 G-C（§5）在测试内自足验证：同一宿主两次 emit 逐字节
> 相同，且 `.kl` 路的重建产物与入库种子 `lib/bootstrap.tabc` 逐字节相同。

## 1. 在管线里的位置

`lib/bootstrap/driver.ta` 的编译半程（`main` 按 `argv[0]` dispatch，三条入口）。
用户面两条入口在 `tinyactor` 包装脚本的 `build` 子命令（`tinyactor help` 同步）：

```
tinyactor build [--emit-kl] <src> [out]   # .ta / .kl 按扩展名分发；out 省略 = 旁边 <base>.tabc
tinyactor build --emit-kl <src.ta> [out.kl]  # 只写 .kl 文本（gcc -S 语义，无字节码）
tinyactor run <src> [args...]             # .ta / .kl / .tabc 按扩展名分发（.tabc = 直跑镜像，不编译）
```

flag 统一前置，之后是 `<src> [out]`。它们落到 driver 的三条入口（包装脚本
负责宿主/缓存约定与参数顺序翻译）：

```
driver.ta <src> <out> <cache_dir>            # .ta 路（原样：make bootstrap / test-bootstrap）
driver.ta --emit-kl <kl> <src> <out> <cd>    # .ta 路 + 多写一份 .kl（写完仍走原路）
driver.ta --from-kl <kl> <out>               # .kl 路（不读源码、不跑 typecheck）
```

**已知怪癖（不修 driver，包装层吸收）**：driver 用 `argv[0]` 分派 ⇒ `--emit-kl` /
`--from-kl` **必须排在位置参数之前**；`<src>` 顶到 `argv[0]` 就落进 `.ta` 路、flag 被
当成文件名（`--from-kl <kl> <out>` 形态更硬：它**没有** `<src>` 位置参数）。用户面
flag 统一前置（`tinyactor` 的 `build` 分发只接受前置 `--emit-kl`），driver 调用
保持 flag 在前的约定（`tinyactor` 的 `build_tavm`）。

```ta
let low = lower-ast.lower_program(forms)                 // .ta 路
let low = kl.read_program(file.read(kl_path))            // .kl 路（内容是 lower 的文本）
// 两条路在这里汇合（compile_low）：不存在"只改了一条路"的可能
match kl.read_program(bytecode.lib_src()) {              // 前奏本身就是 .kl 词表里的东西
  Err(msg) -> die(str.concat("prelude parse: ", msg))
  Ok(prelude) -> match compile.compile(list.append(prelude, low)) {
    Fail(msg) -> die(...)
    Out(fns, consts) -> bytecode.write_bc(out, fns, consts)
  }
}
```

- `--emit-kl` **不参与编译**：它只多写一份文本，产物逐字节不变（G-B 的一半判据）。
- 入库种子与快照都是 `make bootstrap` 的产物（同一条 recipe）：`.ta` 源码动了
  而产物没跟着重生成 ⇒ G-C/G-D 红。
- 前处理（tokenize → parse → resolve_imports → typecheck）在 `.kl` 路径上**不跑**，
  因为生成 `.kl` 时已经跑过 ⇒ `.kl` 路径**不做 typecheck**（结构性事实，见 §1.1）。
- 前奏 `bytecode.lib_src()`（一行 lisp 形文本）两条路共享，它本身就是 `.kl` 词表里的
  文本 ⇒ 用**同一个** reader 读（`kl.read_program`），编译路径对 C reader 的调用到此为止。
- 两条不变式（都落在测试里）：
  **`write(read(write(T))) == write(T)`**（文本幂等，G-A）
  与 **`read(write(T)) ≡ T`**（树等价 ⇒ 同 bytecode，G-B）。

### 1.1 `.kl` 路不跑 typecheck —— 结构性事实，不是待修的缺陷

`typecheck_cached(forms, ...)` 跑在 **parser AST** 上；而 `lower` 丢掉 `type` / `type-sig` /
`const`（`inert_form`，`lower-ast.ta:361-365`）。所以：

- `.kl` 路**没有类型信息可查**，也不跑 `non-exhaustive` 这类依赖臂信息的门（它们报告的是
  parser AST 的臂信息，而 `.kl` 里没有臂信息）；
- 手写 `.kl` 绕过全部静态检查：不要试图给 `.kl` 补一层 typechecker——那要把 lower 丢掉的
  东西重新背回格式里，等于把已移除的缺陷加回来；
- 所以 `.kl` 是**编译器产物格式**（可读、可手写，用于调试/教学），**不是**新的用户语言入口：
  `tinyactor build --from-kl`（§1）是它的入口，但它**不替代编 `.ta`**（无 typecheck，格式
  也不承诺跨版本稳定）；
- 这条路径的诚实性不靠 typecheck，靠三件事：格式门 **G-A**（可往返）、等价门 **G-B**
  （同产物，cmp）、以及坏输入必须响亮红（缺文件 / 语法错 → 非零退出**且不落半个 `.tabc`**，
  `run_bootstrap_tests.sh` 第 4 项）。

## 2. 词表（= lower 产物的词表，闭合）

7 个特殊词形 + 通用调用 + 原子 + 点对：

| 词形 | 形状 |
|---|---|
| `define` | `(define NAME (lambda PARAMS BODY))` |
| `lambda` | `(lambda PARAMS BODY...)`（可能多一个 `ret_type` 槽，compile 忽略） |
| `let` | `(let (NAME INIT) BODY...)` |
| `if` | `(if COND THEN ELSE)` |
| `begin` | `(begin E1 E2 ...)` |
| `quote` | `(quote ATOM)`，writer 写 `'ATOM`（见 §3） |
| `receive` | `(receive (PATTERN [GUARD] BODY...) ...)` |
| 调用 | `(F A1 ... An)`，`F` 可以是任意表达式 |
| 原子 | int（int48）/ float（`1.5`，见 §3）/ `"string"` / `true` / `false` / `nil` / symbol |
| 点对 | `(A . B)` —— `B` 不是列表时（见 §2.1） |

### 2.1 点对是词表的一部分（不是防御性分支）

点对仍在文法与 reader 契约里（`lib/sexp.c` 的 `dotted` 规则；kl-roundtrip 的拼写表
也仍钉着 `(pair . rest)`）：writer 是通用序列化器，任何树都要能往返。但**真实的
driver 闭包里已经没有点对尾巴了**——match desugar 进 lower 期后，`cons(h, t)` 模式
不再以 `(pair . rest)` 的形态出现在 `.kl`（见 §2.2 的对照；match 词形整体离开了
词表）。历史注：C1 时代的 `test/kl-driver-roundtrip.ta` 曾实测旧 `.kl` 里 match 节点
165 处、点对尾 50 处，作为当时的往返证据；该 harness 已随 C2 删除。

### 2.2 教学对照：match 源 vs desugar 后的 `.kl`

match desugar 已前移到 lower 期（`lower-ast.match_desugar`，compile 侧不再认 match
词形），`.kl` 里只有脱糖后的 `let`/`if`/`eq?`/`pair?`/`car`/`cdr`。两个实测样本
（`--emit-kl` 生成，非手写）：

TA 源：

```ta
fn sum(xs) {
  match xs {
    [] -> 0,
    [a | rest] -> a + sum(rest)
  }
}
```

`.kl`（desugar 后）：

```lisp
(let (_m xs)
  (if (eq? _m nil)
    0
    (if (if (pair? _m) (if true true))
      (let (rest (cdr _m))
        (let (a (car _m)) (+ a (sum rest))))
      nil)))
```

读法：scrut 只求值一次，存进 `_m` 槽；字面量臂 = `(eq? _m lit)`；cons 臂 =
pair? 判定 + cdr/car 逆序绑定（rest 先于 a，与模式从外到里一致）；无兜底臂时
值为 `nil`。

可运行的完整教学样本在 `test/kl/match.kl`（历史 `match.lisp`@974703b^ 五个臂
——字面量 / 变量兜底 / `_` 通配 / 无匹配 → nil / 嵌套 pair——的 desugar 后
正例，期望输出 `(20 14 99 nil 3)`，由 `make test-kl-sem` 守着）。

带守卫的臂（guard 包住臂体，未过守卫落到下一个臂）：

```lisp
(let (_m xs)
  (if (if (pair? _m) (if true (if (pair? (cdr _m)) (if true (eq? (cdr (cdr _m)) nil)))))
    (let (g (car (cdr _m)))
      (let (h (car _m))
        (if (> g 0) (print h) (if true (print "other") nil))))
    (if true (print "other") nil)))
```

## 3. 拼写规则（writer 是唯一事实来源）

1. **引号原子一律写 `'NAME`**，不写 `(quote NAME)`；非符号载荷保持显式
   `(quote X)` —— 因为 `'42` 按名字读成**符号** 42，而 `(quote 42)` 是 int 42 的引号，
   两者不能混写（同一 token 在裸位置读**值**，在 `'` 后读**名字**）。
2. **浮点常量写裸 `1.5`**，不写 `(float "1.5")`：与 int 同款，一个 token 就是
   一个字面量（`('float "1.5")` 节点；载荷是**文本**，double 由 VM 装载期 `strtod`
   做——编译器里没有 float 值）。`'1.5` 则是**符号** `1.5`（同规则 1 的 `'42`）——
   只有 `'` 后是名字，显式 `(quote 1.5)` 形式里 X 在非引号位置，仍是字面量。
   载荷不是浮点词法时（手写的 `(float "x")`）退回通用列表拼法：写成裸 token
   读不回来（`read(write(T)) == T` 往返不变式当场破）。
3. **字符串只转义** `\" \\ \n \t \r`（= reader 的解码集）。其余字节逐字保留
   （不可逆转义会破坏幂等）。
4. **其余 verbatim**：writer 是**序列化器不是 pass** —— 不排序、不去重、不折叠
   `begin`、不动 `ret_type` 槽、不脱 `match`。match 在更早的 **lower 期**就由
   `lower-ast.match_desugar` 脱糖终结（compile 侧不再认 match 词形），所以 `.kl`
   里本就没有 match —— writer 自然无事可做。顶层 form 顺序是语义（fnid /
   闭包槽序依赖它）。
5. **布局不是语义**：顶层 form 之间空一行；单 form（含缩进）不超过 78 列就单行，
   否则列表按元素断行（头元素留在 `(` 同一行，其余各占一行、缩进 +2）。
   reader 忽略空白，布局只为 diff/评审确定性。
6. **已知表达边界**：符号的 token 形状必须与 TA 标识符/数字不混淆才能出现在
   **非引号位置**；点对标记 `.` 只在元素链**非空**时是尾标记（首元素位置的 `.`
   仍是普通符号）。lower 产物的符号都来自源码 token，落在边界内；守卫是 G-A/G-B。

## 4. reader 契约（C3 起 = `lib/bootstrap/kl.ta`；`lib/sexp.c` Grammar v1 仍在原处）

同一套文法、同一套词表，两个实现：`lib/sexp.c`（前台工具 / 运行期）与
`lib/bootstrap/kl.ta`（编译路径）。**错误文本逐字相同**（含 `line L, col C` 的位置
语义）——`test/compiler/kl-roundtrip.ta` 直接断言 `sexp.err() == kl 的错误串`。

```text
form    := atom | list | quote
list    := '(' form* ')'          — 空表是 nil
dotted  := '(' form+ '.' form ')' — 点对，如 (a . b)；(a . (b c)) 就是 (a b c)
quote   := '\'' form              — 'x 读成 (quote x)
atom    := int48 | float | symbol | string
comment := ';' 到行尾
```

- **`'` 后跟原子 token ⇒ 原样 intern 成符号**（不做 int/`nil`/`true`/`false` 映射）：
  `'nil` 是符号 `nil`、`'42` 是符号 42。这才让"每个符号都写得出来"——否则符号
  `nil`/`true`/`false` 在池里与同名**值**合并（实测 driver 闭包 `1211 → 1208` 个
  const、少 78 B）。`'` 后跟列表/字符串仍递归解析：`'(a b)` → `(quote (a b))`。
- **`.` 在元素位置 ⇒ 点对尾**；前无元素时是普通符号。
- **浮点字面量 ⇒ `('float "1.5")`**（与 parser 同形状：载荷是字面量**文本**，double
  由 VM 装载期的 `strtod` 做）。`.` 两侧都要有数字：`1.` / `.5` / `1.5.6` 不是浮点
  词法 ⇒ 仍是符号。这是与 `lib/sexp.c` 的行为差异之一：C reader 把 `1.5` 读成**符号**，
  那符号进编译器会被当模块限定名拆开、一路走到装载期才炸（#375）；C3 起编译路径只用
  本 reader，所以这条偏差只落在「手写 `.kl`」这一侧。
- **严格性（与 C reader 的行为差异之一）**：`kl.read` 只吃**一个** form（其后只允许
  空白与 `;` 注释），`kl.read_program` 吃若干顶层 form（空文本 = 空程序）。C reader 的
  `sexp.parse` 只做后者且允许尾随垃圾 ⇒ `.kl` 入口把 `(a) (b)` 当**错误**，不静默
  丢掉半个文件。
- **int 界对称**：接受 `[-140737488355327, 140737488355327]`（与 `lib/json.ta` 的
  `int_acc` 同一条规则）。int48 的真下界 `-2^47` 读不回来：它的绝对值 `2^47` 本身就
  越界，TA 里连中间量（逐位累加的 `acc`）都算不出来 —— 硬要接受它，累加过程就已经
  回绕成**错的常量**，那正是"读者绝不产出错的树"要拦的。C reader 用 `strtoll` 能多
  接受这一个值（本节的第二处有意偏差），两侧都有断言（`kl-roundtrip.ta` 的严格边界块）。
- 硬错误（`Err` 携带 `line L, col C: …`）：不配对的括号、悬空 `.` 尾、`.` 尾后还有
  元素、int48 越界、未知转义、未终结字符串、符号 > 255 字符、整数字面量 > 31 字符。
  词表/范围类错误的位置取 **token 末尾**（与 `lib/sexp.c` 的 `parse_atom` 一致：它
  先扫完 token 再报）。**arena 耗尽是 fatal abort**，不是返回 nil（C0/#281：缺
  `lib/sexp.dylib` 时响亮报错，不再静默劣化）。

## 5. 门

| 门 | 命题 | 怎么跑 |
|---|---|---|
| **G-A** | 格式可往返（文本幂等 + 逐字节不动点）：拼写表 + 断行 + 真实 lower 树 + 前奏文本无引号；C3 起另加"TA reader 与 C reader 逐条等价 + 响亮失败表 + 严格边界" | `bash test/run_compiler_tests.sh`（`test/compiler/kl-roundtrip.ta`，280 断言） |
| **G-B** | 两条路同产物：`compile(lower(driver.ta))` vs `compile(read(write(lower(driver.ta))))` 逐字节（含 `.tabc` 头两数） | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh`（第 3 项：`--emit-kl` 写 → `--from-kl` 读回 → `cmp`） |
| **G-C** | `.kl` 自足门（Phase D 锚，测试内自足、不靠入库快照）：① 同一宿主二次 emit 与首次逐字节相同（writer 确定性）；② `.kl` 路的 from-kl 重建产物与入库种子逐字节相同（连上 G-D 的 rebuilt == seed，三路闭合成一格） | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh`（第 5 项；宿主 = G-D 刚判过的重建产物） |
| **G-D** | 既有 fixed point：`tavm -q $seed driver.ta rebuilt ""` 后 `cmp rebuilt $seed` | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh` |
| **G-E** | 逐模块 `.kl` 链接 == 整 `.kl` 逐字节 + `--emit-kl-dir` 不影响编译产物 + `--from-kl` 闭环（52 程序：driver 闭包 + 全部 lib 单元各自成程序） | `make test-kl-link` / `sh test/run_kl_link_gate.sh`（§9） |
| **V5** | 全仓 0 处依赖旧引号读法（`'nil`/`'true`/`'false`/`'<数字>`） | `bash test/run_compiler_tests.sh`（grep 断言） |

**`driver.kl` 不入 kernfuzz 冻结面**（C4 判断）：冻结面 = `tools/kernfuzz/snapshot.scm`
的扫描根（`test/basic/*.ta` + `test/compiler/*.ta`）里每个 `*.ta` 的 parser AST 快照，
锁的是 **parser 契约**（kernfuzz 的变异目标）；`driver.kl` 既不在那些目录、也不是 `.ta`，
而且它是 **lower 之后** 的产物 —— 与 parser 契约无关。它的等价性由 G-B/G-C 的逐字节
`cmp` 覆盖（比文本 AST 快照强），再塞进冻结面只会给"重生成"多加一道手工审批，
和种子那套纪律（`make bootstrap` 一趟出、禁手合）冲突。


C1 的过渡 harness（`test/kl-driver-roundtrip.ta`：单进程、直接调 driver 的公开半程）
在 C2 随接缝落地**删除**：它存在的理由是"接缝还不存在时 G-B 也得能红/绿"，接缝一旦是真的
CLI（`--emit-kl` / `--from-kl`），同一命题就不该留第二份实现。它曾独占的两条断言已搬家：
`bytecode.lib_src()` 前奏无引号 → `test/compiler/kl-roundtrip.ta` §4；writer 纯度
（无 desugar 开关）一直由 G-A 拼写表覆盖。

**V1–V6 状态**（设计文档 §4.4 的未验证项，本次补齐）：

| # | 项 | 状态 |
|---|---|---|
| V1 | verbatim 的往返 | ✅ writer 无 desugar 开关（纯序列化器），G-A 拼写表断言 `begin`/`receive` 原样保留、match 以脱糖后形态往返（desugar 已前移到 lower，见 §2.2）；C1 过渡 harness 实测旧 `.kl` match 节点 165 → 165（该 harness 已随 C2 删除，数字属 C1 证据），现在由 G-A + G-B 的 cmp 覆盖 |
| V2 | C3 的 TA reader 原型 | ✅ 落地为 `lib/bootstrap/kl.ta` 的 `read` / `read_program`：与 C reader 的等价表（规范文本 + 错误文本逐条相同）、`-2^47` 唯一偏差、严格性（尾随垃圾）与 `read`/`read_program` 的分工都在 `kl-roundtrip.ta` 里；编译路径已换过去（case (i) 的隐藏 dylib 断言） |
| V3 | `.kl` 里的 `(quote (a b))` 引号列表 | ✅ G-A 拼写表两行：`(quote (a b))` 正例 + `(quote 42)` 反例（不能写成 `'42`） |
| V4 | arena 峰值（见 §6） | ✅ 实测（C3 树）：三条路 floor 同在 36 MiB 量级——`.kl` 路跳过 typecheck（内存大头），所以"文本 + 整树"不比 `.ta` 路更贵 |
| V5 | 旧引号行为依赖 = 0 | ✅ grep 断言（2 半：`sexp.parse("'…")` 字面量调用点 + `lib/*.ta` 里的 `(quote nil\|true\|false)`）；运行期唯一输入（`bytecode.lib_src()` 前奏）实测无 `'`（断言在 `test/compiler/kl-roundtrip.ta` §4）。冻结语料 `test/kernfuzz-frozen/*.sexp` **不在 C reader 的消费面上**（读它们的是 `tools/kernfuzz/sexp.py` / `snapshot.scm` 各自的 reader，全仓 0 处 `.ta` 经 `file.read`+`sexp.parse` 读它们），所以不需要第三种断言 |
| V6 | `test/basic` 大闭包的最小 arena | ✅ 实测：`csv-lib`/`http-lib`/`http-serve` 默认 64 MiB arena 全过（run 半程 8 MiB 也够） |

## 6. arena（实测，`TA_ACTOR_HEAP`）

`TA_ACTOR_HEAP` 是**裸字节数**（`ta_inline.h:187`，无 `K/M/G` 后缀，clamp
`[4 KiB, 1 GiB]`，非法值静默保持默认）。**写 `TA_ACTOR_HEAP=1G` 会被整体忽略** ——
那些"用 1G 才过"的记录实际跑的是默认 64 MiB。

| 闸 | floor（本地 macOS/arm64，C3 树重测） | 默认 64 MiB |
|---|---|---|
| G-B `--emit-kl`（单镜像 + 425 KB 文本） | 35 MiB abort / 36 MiB 过 | ✅ 余量 ~1.8x |
| G-B `--from-kl`（单镜像，文本 + 整树） | 35 MiB abort / 36 MiB 过 | ✅ 余量 ~1.8x |
| G-D（单镜像，`tavm -q $seed driver.ta`） | 36 MiB abort / 37 MiB 过 | ✅ 余量 ~1.7x |
| `test/basic/{csv-lib,http-lib,http-serve}` | run 半程 8 MiB 即够 | ✅ |

三条路同量级：typecheck 是内存大头，`.kl` 路跳过它，所以"多一份文本 + 从文本解析的整树"
并不比 `.ta` 路贵——同一内存在 `.ta` 路上本来就要建（lower 之前的那棵树）。

复现（floor 会随树/机器浮动，所以给命令而不是数字）：

```sh
# 先写 .kl（driver 闭包文本；C3 把 TA reader 自己也算进了闭包）
./tavm -q lib/bootstrap.tabc --emit-kl /tmp/driver.kl lib/bootstrap/driver.ta /tmp/A.tabc ""
TA_ACTOR_HEAP=37748736 ./tavm -q lib/bootstrap.tabc --from-kl /tmp/driver.kl /tmp/B.tabc
TA_ACTOR_HEAP=38797312 ./tavm -q lib/bootstrap.tabc lib/bootstrap/driver.ta /tmp/P.tabc ""
```

`TA_ACTOR_HEAP` 只作用**编译宿主自身**（driver 跑在 tavm 上的那半程）。arena 耗尽 = fatal
abort（打印占用明细），绝不返回半个结构；是否需要给编译宿主加显式参数见 issue #308。

## 7. 与设计文档（Phase C `kl-design.md` 评审稿）的偏差

1. **点对必须进词表**（§2.1）：设计文档 §1.1 只说"引号原子"，但 `cons(h, t)` 模式
   在 lower 后就是点对；C reader 原先把 `.` 读成普通符号 ⇒ 树级丢模式（`.tabc` 少
   字节、fns/consts 相同但代码不同）。已补 `dotted` 规则（reader）+ 词表/边界文档。
2. **`TA_ACTOR_HEAP=1G` 是无效拼写**（§6）：设计文档附录 A 的命令按裸字节重述。
3. **"3 个大闭包需 arena knob" 不再成立**（§6）：默认 64 MiB 全过。
4. **G-B 的宿主用重建产物而不是入库种子**（§4.1 的示例命令写的是 `$seed`）：接缝参数是
   **源码**的能力，而入库种子在引入它的那个 PR 里按定义还没有它（要它懂 `--emit-kl`，
   得先 `make bootstrap`——那条链本身就是 G-D）。所以 G-B 用 `run_bootstrap_tests.sh`
   刚重建出来的驱动当宿主，A 路直接复用它（G-D 已判它逐字节 == 种子），同时省掉一次重建。
      G-C 同此宿主与 from-kl 产物，额外再 emit 一次验 writer 确定性。
5. **负 int48 下界收在 `-(2^47-1)`**（§4）：`-2^47` 的绝对值超出 int48，TA 侧根本
   表示不出来（`lib/json.ta:int_acc` 同此规则）；C reader 的 `strtoll` 多接受这一个
   值。偏差面 = 1 个值，两侧各有断言。
6. **渲染器收敛到一份 writer**（设计文档 §3.2 的 #1/#4）：`modsig.sexpr_write/read`
   与 `bridge_test.render*` 删除，改用 `kl.write_flat` / `kl.write_program` /
   `kl.read`。sig cache 文本**逐字节不变**，证据：同一批源码、各自空 cache
   下，老（HEAD 种子）新（C3 种子）两个编译器产出的 `.sig` 文件名与字节全等
   （driver 闭包那趟 16 个，另 5 个 lib 源累积到 23 个，diff = 0）。`write_flat`
   之所以单独存在：真实 sig 行最长 24,147 字符，`write` 的 78 列布局会断行改字节。
   域内 `nil` 的老拼法是 `()`、kl 的规范拼法是 `nil` —— 现有 82 个缓存条目的
   exports 文本里 `nil`/`()` 出现 0 次，所以这处分歧不可观测；即使将来出现，后果
   也只是 sighash 变化导致一次 cache miss（性能），不是正确性（reader 读得回自己
   写出的文本）。
7. **C1 的过渡 harness 随 C2 删除**（`test/kl-driver-roundtrip.ta`，见 §5 末段）：门只留
   一份，跑在真实 CLI 接缝上；它独占的两条断言按"谁的主场归谁"搬走。

## 8. 分阶段（PR 序列）

| PR | 内容 | 状态 |
|---|---|---|
| C0 | `lib/sexp.dylib` 缺失时响亮报错（替换静默 arena 耗尽） | ✅ #314 |
| C1 | `lib/bootstrap/kl.ta` 的 write + reader 引号/点对规则 + G-A/G-B + V1–V6 | ✅ #318 |
| C2 | driver 接缝参数化（`--emit-kl` / `--from-kl`）+ G-B 进 `run_bootstrap_tests.sh` | ✅ #321 |
| C3 | reader 搬编译器层（删编译路径对 `lib/sexp.dylib` 的依赖）+ 渲染器收敛（#1/#4 → kl） | ✅ #325 |
| C4 | `.kl` 快照入仓（`lib/bootstrap/driver.kl`）+ G-C（纪律同 `lib/bootstrap.tabc`：禁手合，重跑再生成） | ✅ 本 PR |
| C5 | `.kl` 快照退为本地/测试产物（不入 git），G-C 改为测试内自足：二次 emit 确定性 + `.kl` 路直连种子 | ✅ |

Phase C 收尾：C0–C5 全绿后 `.ta → .kl → .tabc` 三段各有入口与门（G-A/G-B/G-C/G-D/V5），
`.kl` 稳定性由 G-C 在每趟测试里现场验证——任何后续改动若把 `.kl` 往返或
`compile(driver.kl)` 与种子推开一个字节，G-C 先红。
## 9. 逐模块 `.kl` 与链接（步 2）

`.ta → .kl → .tabc` 的分段再进一步：`.kl` 不只按**整程序**写（`--emit-kl`），
还能按**模块**写。目标形态是逐模块独立编译（每个单元的 `.kl` 单独产出、单独
`--from-kl`），链接器负责拼回整程序。

### 9.1 发射半边：`--emit-kl-dir <dir> <src> <out> <cache>`

`--emit-kl` 的逐模块版：编译**仍走 .ta 路且产物不变**（与 `--emit-kl` 各编
一份 `.tabc`，`cmp` 逐字节相同——分块发射不参与编译），另把 lower 产物按
归属单元分块写 `<dir>/<NN>-<name>.kl` + `<dir>/manifest`（逐行文件名，行序
= 拼接序）。

分块依据不是 lower 产物（lower 内联 const / 丢 inert form / 尾补 main 调用，
块边界已抹掉），而是 `resolve_imports` 的 seq：重放 flat 顺序（`seq_forms`
的 tagged 版）给每个 form 打归属——主文件 forms（含 inert import 标记）归
`main`，`'U` 单元的 ownforms 归单元名——按**末次出现**顺序分块。取末次的原因：
单元 ownforms 在 flat 里连续（首末一致），而主文件的 import 标记会把 `main`
提前到头部，主文件真实 forms 却在 flat 末尾（imports 全在顶部时），逐字节门
要求主文件块排最后。

每块独立过**同一个** `lower_program`（与整程序同一条 lower 路）。逐字节等价
依赖两条结构性事实：TA const 不出口（跨块 const 引用会被 typecheck 拒绝，
所以 per-chunk const 内联 == 整程序 const 内联的逐块限制）；main 尾调用归
主文件块（imports 顶置时恰在 flat 末尾）。任何一条被破坏，门先红。

### 9.2 链接半边：`--link-kl <out> <f1> <f2> ...`

按给定顺序读入各 `.kl` 的顶层 form 表，`list.append` 拼接后整体
`kl.write_program` 重写。**链接不是编译**——没有语义 pass；write 是纯函数，
拼接顺序即字节序。空 `.kl` 是合法输入（纯 `external fn` 声明的单元整程序
lower 后就是空文本，如 `lib/str.ta`），贡献零 form。

### 9.3 回归门（G-E）

对 52 个程序（driver 闭包 + 每个 `lib/*.ta` / `lib/bootstrap/*.ta` 各自成
程序）：① `--emit-kl-dir` 与 `--emit-kl` 各编的 `.tabc` 逐字节相同（分块
发射不影响编译）；② `--link-kl` 按 manifest 顺序拼接 == `--emit-kl` 整
`.kl` **逐字节**；③ `--from-kl` 闭环：linked 与 whole 各编的 `.tabc` 逐
字节相同。

```
make test-kl-link      # 已接进 make test 链（双 OS test job 均看守）
```

CLI 面提案：`--emit-kl-dir` / `--link-kl` 与 `--emit-kl` / `--from-kl` 同层
（driver dispatch 参数，见 driver.ta `main`）。用户面 `tinyactor` 前端暂不
暴露——等逐模块独立编译（每单元 `.kl` 单独 `--from-kl`）落地后再定前端形状。

### 9.4 `.kl` 语义正例（步 3）

`test/kl/*.kl` + 同名 `.expected`：fib / collatz / collatz1m / map / quote /
list / closure 七个是 `974703b^:test/backend/*.lisp` 的原样复活（它们本就是
desugar 后词表的手写正例），match.kl 是同源 `match.lisp` 的脱糖教学对照
（§2.2），float.kl 是浮点拼法的正例（裸 `1.5` 读成 float **值**：`float?` 为真、
`(+ 1.5 2.5)` 走浮点，见 §3 规则 2 / #375）。每条测试：`--from-kl` 编译 → `tavm`
运行 → stdout 与 `.expected` 逐字节 cmp。`.kl` 路不跑 typecheck（§1.1），所以这组测试守的是
reader + compile_low + 前奏的语义面；负例归 kernfuzz / parse-errors 侧。
`make test-kl-sem`（已接进 `make test` 链）。
