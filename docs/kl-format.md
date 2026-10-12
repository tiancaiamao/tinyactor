# `.kl` 中间格式（Phase C）

> `.ta → .kl → .tabc`。`.kl` 是**编译管线中间层**：不是新 runtime、不是用户语言、
> 也不是对外/跨版本稳定格式。内容 = `lower_program` 产物的文本序列化，reader 直接
> 复用 `lib/sexp.c`（`sexp.parse`）——**没有第二套读法**，所以不可能两套语义漂移。
>
> writer: [`lib/bootstrap/kl.ta`](../lib/bootstrap/kl.ta)（`write` / `write_program`）
> reader: [`lib/sexp.c`](../lib/sexp.c) 的 Grammar v1（诚实契约见 [`lib/sexp.ta`](../lib/sexp.ta)）

## 1. 在管线里的位置

`lib/bootstrap/driver.ta` 的编译半程（约 `driver.ta:1764`）：

```ta
let low = lower-ast.lower_program(forms)                 // .ta 路
let low = sexp.parse(file.read(kl_path))                 // .kl 路（内容等价）
match compile.compile(list.append(sexp.parse(bytecode.lib_src()), low)) {
  Fail(msg) -> die(...)
  Out(fns, consts) -> bytecode.write_bc(out, fns, consts)
}
```

- 前处理（tokenize → parse → resolve_imports → typecheck）在 `.kl` 路径上**不跑**，
  因为生成 `.kl` 时已经跑过 ⇒ `.kl` 路径**不做 typecheck**（门禁靠 G-B 的逐字节等价）。
- 前奏 `bytecode.lib_src()`（一行 lisp 形文本）两条路共享，它本身就是 `.kl` 词表里的文本。
- 两条不变式（都落在测试里）：
  **`write(read(write(T))) == write(T)`**（文本幂等，G-A）
  与 **`read(write(T)) ≡ T`**（树等价 ⇒ 同 bytecode，G-B）。

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
| **G-A** | 格式可往返（文本幂等 + 逐字节不动点）：拼写表 + 断行 + 真实 lower 树 | `bash test/run_compiler_tests.sh`（`test/compiler/kl-roundtrip.ta`，76 断言） |
| **G-B** | 两条路同产物：`compile(lower(driver.ta))` vs `compile(read(write(lower(driver.ta))))` 逐字节 | 同上（`test/kl-driver-roundtrip.ta`，~10s，`.tabc` 头 `617 1211`） |
| **G-C** | `.kl` 快照单独重建种子（Phase D 锚，**未建**） | 需要 `.kl` 入仓（C4） |
| **G-D** | 既有 fixed point：`tavm -q $seed driver.ta rebuilt ""` 后 `cmp rebuilt $seed` | `make test-bootstrap` / `bash test/run_bootstrap_tests.sh` |
| **V5** | 全仓 0 处依赖旧引号读法（`'nil`/`'true`/`'false`/`'<数字>`） | `bash test/run_compiler_tests.sh`（grep 断言） |

**V1–V6 状态**（设计文档 §4.4 的未验证项，本次补齐）：

| # | 项 | 状态 |
|---|---|---|
| V1 | verbatim（`match` 不脱糖）的往返 | ✅ 实测：writer 无 desugar 开关，G-B 就是 verbatim 形态；match 节点数 165 → 165，逐字节相同 |
| V2 | C3 的 TA reader 原型 | ⏳ 属 C3（本 PR 仍复用 `lib/sexp.c`） |
| V3 | `.kl` 里的 `(quote (a b))` 引号列表 | ✅ G-A 拼写表两行：`(quote (a b))` 正例 + `(quote 42)` 反例（不能写成 `'42`） |
| V4 | arena 峰值（见 §6） | ✅ 实测：G-B 门 floor ≈ 47 MiB；G-D 门 ≈ 44 MiB |
| V5 | 旧引号行为依赖 = 0 | ✅ grep 断言（2 半：`sexp.parse("'…")` 字面量调用点 + `lib/*.ta` 里的 `(quote nil\|true\|false)`）；运行期唯一输入（`bytecode.lib_src()` 前奏）实测无 `'`。冻结语料 `test/kernfuzz-frozen/*.sexp` **不在 C reader 的消费面上**（读它们的是 `tools/kernfuzz/sexp.py` / `snapshot.scm` 各自的 reader，全仓 0 处 `.ta` 经 `file.read`+`sexp.parse` 读它们），所以不需要第三种断言 |
| V6 | `test/basic` 大闭包的最小 arena | ✅ 实测：`csv-lib`/`http-lib`/`http-serve` 默认 64 MiB arena 全过（run 半程 8 MiB 也够） |

## 6. arena（实测，`TA_ACTOR_HEAP`）

`TA_ACTOR_HEAP` 是**裸字节数**（`ta_inline.h:187`，无 `K/M/G` 后缀，clamp
`[4 KiB, 1 GiB]`，非法值静默保持默认）。**写 `TA_ACTOR_HEAP=1G` 会被整体忽略** ——
那些"用 1G 才过"的记录实际跑的是默认 64 MiB。

| 闸 | floor（本地 macOS/arm64） | 默认 64 MiB |
|---|---|---|
| G-B（两镜像同进程） | 46 MiB abort / 47 MiB 过 | ✅ 余量 ~1.36x |
| G-D（单镜像，`tavm -q $seed driver.ta`） | 40 MiB abort / 44 MiB 过 | ✅ 余量 ~1.45x |
| `test/basic/{csv-lib,http-lib,http-serve}` | run 半程 8 MiB 即够 | ✅ |

复现：`TA_ACTOR_HEAP=50331648 ./tinyactor run test/kl-driver-roundtrip.ta`。
编译半程（driver 自身）由 `tinyactor` 包装器 `env -u TA_ACTOR_HEAP` 跑，knob 只
作用运行半程。arena 耗尽 = fatal abort（打印占用明细），绝不返回半个结构；
是否需要给编译宿主加显式参数见 issue #308。

## 7. 与设计文档（Phase C `kl-design.md` 评审稿）的偏差

1. **点对必须进词表**（§2.1）：设计文档 §1.1 只说"引号原子"，但 `cons(h, t)` 模式
   在 lower 后就是点对；C reader 原先把 `.` 读成普通符号 ⇒ 树级丢模式（`.tabc` 少
   字节、fns/consts 相同但代码不同）。已补 `dotted` 规则（reader）+ 词表/边界文档。
2. **`TA_ACTOR_HEAP=1G` 是无效拼写**（§6）：设计文档附录 A 的命令按裸字节重述。
3. **"3 个大闭包需 arena knob" 不再成立**（§6）：默认 64 MiB 全过。

## 8. 分阶段（PR 序列）

| PR | 内容 | 状态 |
|---|---|---|
| C0 | `lib/sexp.dylib` 缺失时响亮报错（替换静默 arena 耗尽） | ✅ #314 |
| C1 | `lib/bootstrap/kl.ta` 的 write + reader 引号/点对规则 + G-A/G-B + V1–V6 | ✅ 本 PR |
| C2 | driver 接缝参数化（`--emit-kl` / `--from-kl`）+ G-B 进 `run_bootstrap_tests.sh` | 未做 |
| C3 | reader 搬编译器层（删编译路径对 `lib/sexp.dylib` 的依赖）+ 渲染器收敛 | 未做 |
| C4 | `.kl` 快照入仓 + G-C（纪律同 `lib/bootstrap.tabc`：禁手合，重跑再生成） | 未做 |