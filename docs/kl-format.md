# `.kl` 中间格式（Phase C）

> `.ta → .kl → .tabc`。`.kl` 是**编译管线中间层**：不是新 runtime、不是用户语言、
> 也不是对外/跨版本稳定格式。内容 = `lower_program` 产物的文本序列化，reader 直接
> 复用 `lib/sexp.c`（`sexp.parse`）——**没有第二套读法**，所以不可能两套语义漂移。
>
> writer: [`lib/bootstrap/kl.ta`](../lib/bootstrap/kl.ta)（`write` / `write_program`）
> reader: [`lib/sexp.c`](../lib/sexp.c) 的 Grammar v1（诚实契约见 [`lib/sexp.ta`](../lib/sexp.ta)）

## 1. 在管线里的位置

`lib/bootstrap/driver.ta` 的编译半程（`main` 按 `argv[0]` dispatch，三条入口）：

```
driver.ta <src> <out> <cache_dir>            # .ta 路（原样：make bootstrap / test-bootstrap）
driver.ta --emit-kl <kl> <src> <out> <cd>    # .ta 路 + 多写一份 .kl（写完仍走原路）
driver.ta --from-kl <kl> <out>               # .kl 路（不读源码、不跑 typecheck）
```

```ta
let low = lower-ast.lower_program(forms)                 // .ta 路
let low = sexp.parse(file.read(kl_path))                 // .kl 路（内容等价）
// 两条路在这里汇合（compile_low）：不存在"只改了一条路"的可能
match compile.compile(list.append(sexp.parse(bytecode.lib_src()), low)) {
  Fail(msg) -> die(...)
  Out(fns, consts) -> bytecode.write_bc(out, fns, consts)
}
```

- `--emit-kl` **不参与编译**：它只多写一份文本，产物逐字节不变（G-B 的一半判据）。
- 前处理（tokenize → parse → resolve_imports → typecheck）在 `.kl` 路径上**不跑**，
  因为生成 `.kl` 时已经跑过 ⇒ `.kl` 路径**不做 typecheck**（结构性事实，见 §1.1）。
- 前奏 `bytecode.lib_src()`（一行 lisp 形文本）两条路共享，它本身就是 `.kl` 词表里的文本。
- 两条不变式（都落在测试里）：
  **`write(read(write(T))) == write(T)`**（文本幂等，G-A）
  与 **`read(write(T)) ≡ T`**（树等价 ⇒ 同 bytecode，G-B）。

### 1.1 `.kl` 路不跑 typecheck —— 结构性事实，不是待修的缺陷

`typecheck_cached(forms, ...)` 跑在 **parser AST** 上；而 `lower` 丢掉 `type` / `type-sig` /
`const`（`infer_form`，`lower-ast.ta:361-365`）。所以：

- `.kl` 路**没有类型信息可查**，也不跑 `non-exhaustive` 这类依赖臂信息的门（它们报告的是
  parser AST 的臂信息，而 `.kl` 里没有臂信息）；
- 手写 `.kl` 绕过全部静态检查：不要试图给 `.kl` 补一层 typechecker——那要把 lower 丢掉的
  东西重新背回格式里，等于把已移除的缺陷加回来；
- 所以 `.kl` 是**编译器产物格式**（可读、可手写，用于调试/教学），**不是**受支持的用户入口；
- 这条路径的诚实性不靠 typecheck，靠三件事：格式门 **G-A**（可往返）、等价门 **G-B**
  （同产物，cmp）、以及坏输入必须响亮红（缺文件 / 语法错 → 非零退出**且不落半个 `.tabc`**，
  `run_bootstrap_tests.sh` 第 4 项）。

## 2. 词表（= lower 产物的词表，闭合）

8 个特殊词形 + 通用调用 + 原子 + 点对：

| 词形 | 形状 |
|---|---|
| `def` | `(def NAME (lambda PARAMS BODY))` |
| `lambda` | `(lambda PARAMS BODY...)`（可能多一个 `ret_type` 槽，compile 忽略） |
| `let` | `(let (NAME INIT) BODY...)` |
| `if` | `(if COND THEN ELSE)` |
| `begin` | `(begin E1 E2 ...)` |
| `quote` | `(quote ATOM)`，writer 写 `'ATOM`（见 §3） |
| `receive` | `(receive (PATTERN [GUARD] BODY...) ...)` |
| `match` | `(match SCRUT (PAT [GUARD] BODY...) ...)`（**不脱糖**，见 §3.3） |
| 调用 | `(F A1 ... An)`，`F` 可以是任意表达式 |
| 原子 | int（int48）/ `"string"` / `true` / `false` / `nil` / symbol |
| 点对 | `(A . B)` —— `B` 不是列表时（见 §2.1） |

### 2.1 点对是词表的一部分（不是防御性分支）

`cons(h, t)` 这类**模式**在 lower 期摊平成点对：`lib/bootstrap/typecheck.ta` 的

```ta
match lst { nil -> nil  cons(pair, rest) -> ... }
```

降级后是 `(match lst (nil nil) ((pair . rest) ...))` —— 即 car=`pair`、cdr=符号
`rest` 的**点对**，不是三元列表。读成 `(pair . rest)` 会把模式语义读丢，所以 reader
必须认这个形态（`lib/sexp.c` 的 `dotted` 规则）。driver 全闭包里这类尾有 165 处
（`test/kl-driver-roundtrip.ta` 断言 match 节点数往返不丢）。

## 3. 拼写规则（writer 是唯一事实来源）

1. **引号原子一律写 `'NAME`**，不写 `(quote NAME)`；非符号载荷保持显式
   `(quote X)` —— 因为 `'42` 按名字读成**符号** 42，而 `(quote 42)` 是 int 42 的引号，
   两者不能混写（同一 token 在裸位置读**值**，在 `'` 后读**名字**）。
2. **字符串只转义** `\" \\ \n \t \r`（= reader 的解码集）。其余字节逐字保留
   （不可逆转义会破坏幂等）。
3. **其余 verbatim**：writer 是**序列化器不是 pass** —— 不排序、不去重、不折叠
   `begin`、不动 `ret_type` 槽、**不脱 `match`**。脱糖是 `compile.match_desugar` 的
   职责（在 `.kl` **之后**）。顶层 form 顺序是语义（fnid / 闭包槽序依赖它）。
4. **布局不是语义**：顶层 form 之间空一行；单 form（含缩进）不超过 78 列就单行，
   否则列表按元素断行（头元素留在 `(` 同一行，其余各占一行、缩进 +2）。
   reader 忽略空白，布局只为 diff/评审确定性。
5. **已知表达边界**：符号的 token 形状必须与 TA 标识符/数字不混淆才能出现在
   **非引号位置**；点对标记 `.` 只在元素链**非空**时是尾标记（首元素位置的 `.`
   仍是普通符号）。lower 产物的符号都来自源码 token，落在边界内；守卫是 G-A/G-B。

## 4. reader 契约（`lib/sexp.c` Grammar v1）

```text
form    := atom | list | quote
list    := '(' form* ')'          — 空表是 nil
dotted  := '(' form+ '.' form ')' — 点对，如 (a . b)；(a . (b c)) 就是 (a b c)
quote   := '\'' form              — 'x 读成 (quote x)
atom    := int48 | symbol | string
comment := ';' 到行尾
```

- **`'` 后跟原子 token ⇒ 原样 intern 成符号**（不做 int/`nil`/`true`/`false` 映射）：
  `'nil` 是符号 `nil`、`'42` 是符号 42。这才让"每个符号都写得出来"——否则符号
  `nil`/`true`/`false` 在池里与同名**值**合并（实测 driver 闭包 `1211 → 1208` 个
  const、少 78 B）。`'` 后跟列表/字符串仍递归解析：`'(a b)` → `(quote (a b))`。
- **`.` 在元素位置 ⇒ 点对尾**；前无元素时是普通符号。
- 硬错误（返回 `-1`，细节走 `sexp.err()`）：不配对的括号、悬空 `.` 尾、
  `.` 尾后还有元素、int48 溢出、非字符串实参。**arena 耗尽是 fatal abort**，
  不是返回 nil（C0/#281：缺 `lib/sexp.dylib` 时响亮报错，不再静默劣化）。

## 5. 门

| 门 | 命题 | 怎么跑 |
|---|---|---|
| **G-A** | 格式可往返（文本幂等 + 逐字节不动点）：拼写表 + 断行 + 真实 lower 树 + 前奏文本无引号 | `bash test/run_compiler_tests.sh`（`test/compiler/kl-roundtrip.ta`，78 断言） |
| **G-B** | 两条路同产物：`compile(lower(driver.ta))` vs `compile(read(write(lower(driver.ta))))` 逐字节（`.tabc` 头 `641 1249`） | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh`（第 3 项：`--emit-kl` 写 → `--from-kl` 读回 → `cmp`） |
| **G-C** | `.kl` 快照单独重建种子（Phase D 锚，**未建**） | 需要 `.kl` 入仓（C4） |
| **G-D** | 既有 fixed point：`tavm -q $seed driver.ta rebuilt ""` 后 `cmp rebuilt $seed` | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh` |
| **V5** | 全仓 0 处依赖旧引号读法（`'nil`/`'true`/`'false`/`'<数字>`） | `bash test/run_compiler_tests.sh`（grep 断言） |

C1 的过渡 harness（`test/kl-driver-roundtrip.ta`：单进程、直接调 driver 的公开半程）
在 C2 随接缝落地**删除**：它存在的理由是"接缝还不存在时 G-B 也得能红/绿"，接缝一旦是真的
CLI（`--emit-kl` / `--from-kl`），同一命题就不该留第二份实现。它曾独占的两条断言已搬家：
`bytecode.lib_src()` 前奏无引号 → `test/compiler/kl-roundtrip.ta` §4；`match` 不脱糖属
writer 纯度，一直由 G-A 拼写表覆盖。

**V1–V6 状态**（设计文档 §4.4 的未验证项，本次补齐）：

| # | 项 | 状态 |
|---|---|---|
| V1 | verbatim（`match` 不脱糖）的往返 | ✅ writer 无 desugar 开关（纯序列化器），G-A 拼写表直接断言 `match`/`begin`/`receive` 原样保留；C1 过渡 harness 实测 match 节点 165 → 165（该 harness 已随 C2 删除，数字属 C1 证据），现在由 G-A + G-B 的 cmp 覆盖 |
| V2 | C3 的 TA reader 原型 | ⏳ 属 C3（本 PR 仍复用 `lib/sexp.c`） |
| V3 | `.kl` 里的 `(quote (a b))` 引号列表 | ✅ G-A 拼写表两行：`(quote (a b))` 正例 + `(quote 42)` 反例（不能写成 `'42`） |
| V4 | arena 峰值（见 §6） | ✅ 实测（C2 树）：三条路 floor 同在 34 MiB 量级——`.kl` 路跳过 typecheck（内存大头），所以"文本 + 整树"不比 `.ta` 路更贵 |
| V5 | 旧引号行为依赖 = 0 | ✅ grep 断言（2 半：`sexp.parse("'…")` 字面量调用点 + `lib/*.ta` 里的 `(quote nil\|true\|false)`）；运行期唯一输入（`bytecode.lib_src()` 前奏）实测无 `'`（断言在 `test/compiler/kl-roundtrip.ta` §4）。冻结语料 `test/kernfuzz-frozen/*.sexp` **不在 C reader 的消费面上**（读它们的是 `tools/kernfuzz/sexp.py` / `snapshot.scm` 各自的 reader，全仓 0 处 `.ta` 经 `file.read`+`sexp.parse` 读它们），所以不需要第三种断言 |
| V6 | `test/basic` 大闭包的最小 arena | ✅ 实测：`csv-lib`/`http-lib`/`http-serve` 默认 64 MiB arena 全过（run 半程 8 MiB 也够） |

## 6. arena（实测，`TA_ACTOR_HEAP`）

`TA_ACTOR_HEAP` 是**裸字节数**（`ta_inline.h:187`，无 `K/M/G` 后缀，clamp
`[4 KiB, 1 GiB]`，非法值静默保持默认）。**写 `TA_ACTOR_HEAP=1G` 会被整体忽略** ——
那些"用 1G 才过"的记录实际跑的是默认 64 MiB。

| 闸 | floor（本地 macOS/arm64，C2 树重测） | 默认 64 MiB |
|---|---|---|
| G-B `--emit-kl`（单镜像 + 420 KB 文本） | 32 MiB abort / 34 MiB 过 | ✅ 余量 ~1.9x |
| G-B `--from-kl`（单镜像，文本 + 整树） | 33 MiB abort / 34 MiB 过 | ✅ 余量 ~1.9x |
| G-D（单镜像，`tavm -q $seed driver.ta`） | 33 MiB abort / 34 MiB 过 | ✅ 余量 ~1.9x |
| `test/basic/{csv-lib,http-lib,http-serve}` | run 半程 8 MiB 即够 | ✅ |

三条路同量级：typecheck 是内存大头，`.kl` 路跳过它，所以"多一份文本 + 从文本解析的整树"
并不比 `.ta` 路贵——同一内存在 `.ta` 路上本来就要建（lower 之前的那棵树）。

复现（floor 会随树/机器浮动，所以给命令而不是数字）：

```sh
# 先写 .kl（driver 闭包 = 419,904 B 文本）
./tavm -q lib/bootstrap.tabc --emit-kl /tmp/driver.kl lib/bootstrap/driver.ta /tmp/A.tabc ""
TA_ACTOR_HEAP=34603008 ./tavm -q lib/bootstrap.tabc --from-kl /tmp/driver.kl /tmp/B.tabc
TA_ACTOR_HEAP=34603008 ./tavm -q lib/bootstrap.tabc lib/bootstrap/driver.ta /tmp/P.tabc ""
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
5. **C1 的过渡 harness 随 C2 删除**（`test/kl-driver-roundtrip.ta`，见 §5 末段）：门只留
   一份，跑在真实 CLI 接缝上；它独占的两条断言按"谁的主场归谁"搬走。

## 8. 分阶段（PR 序列）

| PR | 内容 | 状态 |
|---|---|---|
| C0 | `lib/sexp.dylib` 缺失时响亮报错（替换静默 arena 耗尽） | ✅ #314 |
| C1 | `lib/bootstrap/kl.ta` 的 write + reader 引号/点对规则 + G-A/G-B + V1–V6 | ✅ #318 |
| C2 | driver 接缝参数化（`--emit-kl` / `--from-kl`）+ G-B 进 `run_bootstrap_tests.sh` | ✅ 本 PR |
| C3 | reader 搬编译器层（删编译路径对 `lib/sexp.dylib` 的依赖）+ 渲染器收敛 | 未做 |
| C4 | `.kl` 快照入仓 + G-C（纪律同 `lib/bootstrap.tabc`：禁手合，重跑再生成） | 未做 |