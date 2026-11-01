# 模块与 import 设计草案（E2 重构）

> **Status: 草案，未评审、未实现。** 当前实现仍是 [`docs/modules.md`](modules.md)
> 描述的旧契约。本文落地后取代 `modules.md` 成为契约文档；两文的差异即
> 实施清单，见 §9「删除清单」与 §10「迁移四步」。

## 0. 一句话

`import` 只有一种写法：`<树名>:<树内路径>`；**候选恒一条**；**写出的名字就是
模块身份**。

```ta
import "tinyactor:lib/str"                  // → <根>/vendor/tinyactor/lib/str.ta
import "example.org/hello:lib/greet" as g   // 树名可多段
import "tinyactor:lib/bootstrap/parser"      // 编译器自身的源码也是普通树内文件
```

与旧契约的差别只有三句：**没有裸名**、**没有相对路径**、**没有第二根**。
其余全部是删。

---

## 1. 名字

导入名是一个字符串，形态唯一：`<树名>":"<树内路径>`。

1. **树名**：一段或多段（`tinyactor` / `example.org/hello`），段之间 `/`；
   每段非空、非 `.`、非 `..`、不含 `:`。**不要求含点**（旧契约要求首段含点；
   那条规则的存在意义是"无可信注册表时靠域结构保证全局唯一"，而本设计里
   树名只是 `vendor/` 下的一个目录名，唯一性由**目录唯一性**直接提供，见 §4）。
2. **树内路径**：同上段规则，`/` 分隔；**省略 `.ta`**。
3. **只允许一个 `:`**，它就是树名与路径的唯一分界。分界显式，因此
   `example.org/hello:lib/greet` 的树名不可能被读成 `example.org` —— 不需要
   前缀搜索，也不需要"首段含点"来消歧。
4. **卫生违规响亮拒绝**：`error: invalid module name: <name>`。
   负例见 `test/module/import-empty-segment-module-errors.ta`（`a//b`）、
   `test/module/import-dotdot-segment-module-errors.ta`（`a/../b`）。
   **新增：拒绝 `.` 段。** 今天的 `seg_ok`（`driver.ta:736-742`）只拒绝 `..`，
   所以 `import "./b"` 会被拼成 `<dir>/./b.ta` 直接读到文件 —— 实测
   `import "./b"` + `b.hello()` 正常打印 `HELLO`，且同一文件里
   `import "./b"` 与 `import b` 同时存在也不报错（同一个文件因此有**两个名字**，
   这正是 `canon_of_path` 那 22 行要缝的东西）。本设计让这条路径在卫生检查
   处就死掉。

`as n` 语义不变：文件本地别名纪律（一个文件里每个模块只有一个可用拼写）。

## 2. 根

1. **`<根>` = 从入口源文件所在目录向上，第一个含 `vendor/` 的祖先目录。**
   整个构建（一次 `tinyactor run` / `build`）只计算一次。
   - 入口源文件 = CLI 的 `<src>` 实参，已被 `absolute_path` 转成绝对路径。
   - **不读 `$PWD`、不读 `TA_PROJECT_ROOT`（退役）、不读任何环境变量。**
     理由见 §12 否决史最后一条：旧设计把根绑在 `$PWD` 上，实测同一个入口
     文件 `cd test/consumer/app && build main.ta` 报
     `module not found: example.org/consumer:app/screen`，而
     `cd test/consumer && build app/main.ta` 成功。
2. **只有一个根。** 家目录式兜底、安装树兜底、`std:` 保留前缀全部不做
   （否决理由见 §12）。项目需要工具链的标准库时，由
   `tinyactor init` 在项目里建一条普通树链接：

   ```bash
   vendor/tinyactor -> <安装根>        # 相对链接，随安装位置移动仍有效
   ```

   工具链仓库自己用同一条机制指回自己：`vendor/tinyactor -> ../..`。
3. `vendor/` 下**任何目录（含符号链接指向的目录）都是一棵树**，树的"名字"
   就是它在 `vendor/` 下的相对路径。树内部的 `vendor/` 是死数据，永不参与
   解析 —— 这就是"vendor 只一个、依赖平铺"的可执行形式。
4. 找不到 `vendor/` 直接报错（§7），不回退。**这是本设计唯一的硬前置。**
   没有 vendor/ 的一次性脚本要先 `tinyactor init`（一条符号链接，可复制退化）。

## 3. 解析

```ta
resolve(name, root):
    (tree, path) = split_once(name, ":")            // 卫生检查在此前完成
    file = root + "/vendor/" + tree + "/" + path + ".ta"
    content = file.read(file)
    if content == null: error(§7)                    // 候选恒这一条，没有第二发
    return (file, content)
```

- **命中判据 = 一次 `file.read`。** 没有 `exists` 探测、没有候选列表、没有
  顺序、没有遮蔽。
- 名字到文件是**纯函数**：`resolve` 不接收"导入方目录"这个参数（旧
  `find_module_path(name, dir)` 的 `dir` 退役）。
- 入口文件自身不需要被 import；它由 CLI 直接交给编译流程。

## 4. 身份、去重、环

- **单元键 = 写出的名字**（唯一形态 ⇒ 唯一拼写 ⇒ 一个文件在给定构建里
  只有一个名字）。于是：
  - `canon_of_path` / `vendor_lib_rest` / `canon_of_import`（`driver.ta:913-938`，
    共 22 行）、旧契约第 5 条「规范名收敛」、测试
    `test/module/mixed-spelling-converge.ta` 整体退休；
  - 旧契约里的「显示名 / 规范名」二分塌成一层：显示名就是身份。
  - 「内部名」（lowered 限定前缀 + mangle）**不变**，见 §5。
- **去重**：同一名字在一张导入图里只加载一次（菱形依赖天然合流，因为
  菱形两侧写的是同一个名字；旧设计要靠 `canon_of_path` 才成立）。
- **环**：显式检测，`error: circular import: a:b -> a:c -> a:b`（沿用旧实现）。
- 工具的后果：`grep -oE '^import "[^:"]+:' --include=*.ta -r . | sort -u` 直接
  给出本项目的**依赖树集合**，不需要 manifest。反过来，被 vendor 的库文件
  里写着 `hello:lib/greet`，它必须落在 `vendor/hello/` 下才能解析 —— 树的
  名字**由源码自证**，不需要声明文件。

## 5. 内部名（lowered 限定前缀）不变

`qual_prefix(name)`（`driver.ta:714`）原样保留：名字不含 `: . /` 时原样，
否则 mangle 成「末段截 24 字符 + `~` + 名字哈希」。唯一变化是**输入**从
"规范名"换成"写出的名字"，因此：

- **一次性产物变更**：今天 `import str` → 单元名 `str` → 前缀 `str`；
  改造后 `import "tinyactor:lib/str"` → 前缀 `str~<h>`。所有限定符号名都会
  变，`.tabc` 字节随之变。fixed point 门（同一份源码连编两遍逐字节一致）不受
  影响；受影响的是任何**跨版本比对产物**的地方（kernfuzz golden 需重生成）。
- mangle 的三条约束（别按首点切头、kl 标签/分块文件名不能带 `/`、长度有界）
  一条都没变，见旧契约同节。

## 6. 边界：什么不属于 `import`

`import` 只有一个职责：**从树里读一个 `.ta` 文件**。C 提供的函数不是模块，
而是 `external fn` 声明——`lib/os.ta` / `file.ta` / `str.ta` / `buffer.ta` /
`timer.ta` 已经是这个形态（`lib/os.c:19` 的注释写明 `is_builtin_module("os")`
**故意为 false**）。

因此：

- **裸名不再是合法导入名。** `api.c:517` 的硬编码表、`driver.ta` 的
  `is_builtin_import` 短路（`499 / 623 / 656`）、typecheck 的"builtin 模块
  宽松调用路径"（`typecheck.ta:1756-1758`）与 net 的 arity 特例/箭头 promise
  （`typecheck.ta:1100-1103, 3410-3422`，约 28 行）**全部删除**。
- 需要补 **2 个存根**（用 `external fn` 声明，不动 C）：
  - `lib/net.ta`：7 个（`listen/accept/connect(-1)/read(-1)/write/close/errno`，
    与 `lib/net.c:769-779` 的 `net_funcs[]` 一一对应，已核对）；
  - `lib/vm.ta`：10 个（`time_ms/time_us/get_arg/cfunc_index/is_builtin_module/
    parse_source/make_tok_vec/tok_type/tok_val/free_tok_vec`，见
    `src/api.c:789-799`）。
- **唯一例外：`cov`。** 它由 parser 合成（`parser.ta:43-52` 生成
  `(cov.line path line)`），全仓库 **0 处** `import cov`，用户写不出来。
  草案的选择是**保留它为裸名内部协议**，并在文档里标死"编译器合成专用，
  用户面不可写"；长期解法是把行覆盖下沉到 VM 行表（VM 层改动，另开 issue，
  不阻塞本文档）。
- 副产品（都是修复，不是回归）：`vm.*` 今天有 28 个文件在用、只有 1 个文件
  `import vm`（靠那条宽松路径），改造后必须显式 `import "tinyactor:lib/vm"`
  —— 27 行新增，且原来"用了全局"的隐式依赖暴露出来。

## 7. 错误信息

模块不是找到的、是拼出来的，所以报告必须把那一拼**摊开**（这是 Janet 的
报错体验，见 `/tmp/janet_modules.txt` 里的候选清单）：

```
error: module not found: tinyactor:lib/nope
  tried: /proj/vendor/tinyactor/lib/nope.ta
  root:  /proj   (vendor/ at /proj/vendor)
```

没有根时（唯一的"配置型"错误）：

```
error: no vendor/ directory above /tmp/x.ta
  module names resolve as <root>/vendor/<tree>/<path>.ta
  run `tinyactor init` in your project to create one
```

名字违规仍是 `error: invalid module name: <name>`（按写出名报，不补 `.ta`）。

## 8. 为什么"被 vendor 时语义不变"

设库 `hello` 的源码里有 `import "hello:lib/util"` 与 `import "tinyactor:lib/str"`。

- 另一个项目把 `hello` 放到自己 `vendor/hello/` → 两条 import 文字不变，
  `hello:lib/util` 落在新根的 `vendor/hello/lib/util.ta`，
  `tinyactor:lib/str` 落在新根的 `vendor/tinyactor/lib/str.ta`（或它没装就报
  一条明确的 not found）。**没有一条规则依赖"这个文件原来在哪"。**
- 想把 `hello` 改名成 `acme/hello`：只改**放它的目录名**不够（源码里的
  `hello:` 前缀会失配），必须一起改写被 vendor 的源码里的 `hello:` 前缀 ——
  这是**故意的**：改名就是改身份，编译器会逐个 not found 地点名，工具可以
  机械改。旧设计想用"相对路径"绕开这件事，代价是同一份源码在不同位置语义
  不同（正是要消灭的东西）。
- 同一份源码在 `make bootstrap`、`tinyactor run`、`tavm` 直跑三种宿主下解析
  结果一致：**根只由入口文件位置决定**，与谁在跑、cwd 是什么无关。

## 9. 删除清单

### 9.1 `lib/bootstrap/driver.ta`

| 删除 | 行 | 说明 |
|---|---|---|
| `find_module_path` 的 2-4 步搜索 | 805-827 | 只留 `树名:路径` 一条拼接 |
| `helpers/` 隐式目录 | 812-818 | 11 处导入改 `树名:` 显式名 |
| 裸名糖 + `stdlib_rels` 双候选 | 895-910 | 裸名整体退役 |
| `vendor_roots` / `vendor_lookup` / `lookup_in_root` | 842-882 | 单根后不需要遍历 |
| `vendor_identity_path` | 885-892 | 并入唯一解析（同一段代码） |
| `canon_of_path` / `vendor_lib_rest` / `canon_of_import` | 913-938 | 名即身份 |
| `first_seg_has_dot` + 其调用 | 756-770, 773-791 | 树名不再要求含点 |
| `seg_ok` 增加 `.` 段拒绝 | 736-742 | 今天 `./x` 能跑（§1.4 实测） |
| `is_builtin_import` 及三处短路 | 656, 499, 623 | 裸名退役 |
| `find_module_path(name, dir)` 的 `dir` 形参及其上游传递 | 805 / 488 / 543 / 615 / 992 | `grep -c "dir\b"` 目前 82 处，解析相关部分归零 |

净效果：解析带 **≈146 行函数 + 60 行注释 → ≈10 行**。

### 9.2 CLI（`tinyactor`）

- 删 `TA_PROJECT_ROOT="$CALLER_DIR"`（`:161`）；
- 删 `env -u TA_PROJECT_ROOT`（`:117`，自举那处现在没有要丢的东西了）；
- **保留** `cd "$SCRIPT_DIR"`：它服务的是编译期 `dlopen("lib/<mod>.dylib")`
  是 cwd 相对这件事，与模块解析无关（旧契约"编译半程换 CWD"条目同理）。
  要让 cwd 也自由是 loader 的独立议题，不塞进本次。

### 9.3 工具与文档

- `tools/expand_imports.py:36-50` 的 `module_candidates`：4 条候选 → 1 条拼接。
- `docs/modules.md` 整篇被本文取代；`docs/ta-language-spec.md` 的 import
  语法节同步（AGENTS.md 硬要求）。
- 测试：`test/module/mixed-spelling-converge.ta`（混拼写收敛）删除；
  `test/module/import-identity-no-dot-module-errors.ta`（首段无点）删除；
  `test/module/helpers/` 改名为普通子目录或并入显式树路径。

## 10. 迁移四步（每步独立自举、独立 PR）

**过桥原则：先让语料只用一种形态，再删别的形态。** 因为 `树名:路径` 是旧
编译器**已经实现且唯一机械**的路径（`vendor_identity_path`），所以第 2 步
可以只改语料、不动编译器 —— 否则会掉进"新 driver 编不了旧语料、旧种子编不了
新语料"的死锁。

### S1 · 补存根（不动解析）

新增 `lib/net.ta`（7 个 external fn）、`lib/vm.ta`（10 个）；把 20 处 `import net`
与 1 处 `import vm` 改成带点全名 `github.com/tiancaiamao/tinyactor:lib/net`；
给 27 个用 `vm.*` 却没 import 的文件补 import 行。

验证：`make bootstrap` 两遍 + `cmp` 逐字节一致；`make test` 全绿。
（此步完成后 C 模块仍有两条绑定路径，`timer` 的双绑定要在这里实测：
`lib/timer.ta:42-44` 的 3 条 `external fn` 与 `lib/timer.c:296` 的
`vm_register_module("timer",…)` 是同一批符号，`import "…:lib/timer"` 从 no-op
变成真的加载 —— 若行为有变，S1 内先删 `timer.ta` 的重复声明还是删 C 注册，
由实测决定。）

### S2 · 语料全量改名为「带点全名」

按 4 类规则把 **458 条** import 改成
`github.com/tiancaiamao/tinyactor:<树内路径>`（**不动编译器**）：

| 类别 | 条数 | 现状 → 目标 |
|---|---|---|
| 标准库 `lib/*.ta` | 297 | `import str` → `…tinyactor:lib/str` |
| 编译器源码 `lib/bootstrap/*.ta` | 30 | `import tokenizer` → `…tinyactor:lib/bootstrap/tokenizer` |
| 导入方同目录 | 98 | `./lib` 71、`benchmark/compile/src` 20、`test/module/helpers` 4、`benchmark/vs-go` 2、`pkgs-skeleton/_template` 1 |
| `helpers/` 隐式目录 | 11 | `…tinyactor:test/module/helpers/<name>` |
| 保持原样 | 13 | 9 条 identity（已是全名）+ 4 条负例（`a//b`、`a/../b`、`missing_*`） |

**正则改不动的 4 类，必须逐类处理**（实测）：

1. 同文件内相邻行动作相反：`test/consumer/app/main.ta:15-16` 已是全名、
   `:17-19` 是裸名 —— 全文前缀化会把 15-16 改成双重身份。
2. 注释里的 `import <name>`：**199 行**（`grep -rn '^\s*//.*\bimport [a-z]'`），
   按词边界替换会改坏契约说明。
3. 期望 AST 字面量：`test/compiler/parser-ast.ta:468-469` 里写着
   `"import foo"` / `((import \"foo\" \"foo\"))`。
4. `import timer` 这类"从 no-op 变成真加载"的行 —— 语义要实测，不是替换。
   （同样：今天的 `import "./b"` 这类隐性别名按目标文件收敛，不要留下双拼写。）

验证：`make bootstrap` 两遍逐字节一致 + `make test` 全绿。此步之后
**已无任何 import 依赖搜索分支**（这是 S3 可删的前提）。

### S3 · 解析器收窄（单根 + 删搜索）

`driver.ta`：根改为"入口文件的 vendor 祖先"、候选恒一条、删 §9.1 整表；
CLI 停发 `TA_PROJECT_ROOT`；`expand_imports.py` 同步。

同时重造 #368 fixture（`test/consumer/`）：
- 删 `vendor/github.com/tiancaiamao/tinyactor/`（那份 2 文件的"残缺标准库"）；
- 加 `test/consumer/vendor/tinyactor -> ../../..`（指向仓库，拿真标准库）；
- `json` 的覆盖改用**它自己的树**：`vendor/example.org/consumer/lib/json.ta`，
  import 写成 `example.org/consumer:lib/json`（显式、`ls vendor/` 可见）。

`test/run_cli_tests.sh:410-454` 的 5 条 `consumer_check` 与
"无 vendor 项目回退安装树"那条断言按新语义重写（后者变成"无 vendor ⇒
响亮报错 + 提示 `tinyactor init`"）。

验证：`make bootstrap` 两遍 + `make test` + `test/run_cli_tests.sh` 全绿。

### S4 · 树名缩短 + 收尾

- 链接改名 `vendor/github.com/tiancaiamao/tinyactor` → `vendor/tinyactor`；
- 语料 sed `github.com/tiancaiamao/tinyactor:` → `tinyactor:`（含 driver 自身）；
- `first_seg_has_dot` 放宽为"允许无点树名"（与本步一起，因为 S2/S3 之后
  树名才变成单段）；
- 删 native 硬编码表 / `is_builtin_import` / typecheck 宽松路径与 net 特例；
- `docs/modules.md` → 以本文替换；`ta-language-spec.md` 同步。

验证：`make bootstrap` 两遍逐字节一致 + `make test` 11 类 0 failures +
kernfuzz golden 重生成。

## 11. 实测依据（2026-11 本轮统计，排除 `vendor/` 与 `build/`）

```
import 总数 471，197 个文件
  identity 形态（含 `:`）                     9     ← 保持不变
  落 lib/*.ta（标准库）                     297
  落 lib/bootstrap/*.ta（编译器源码）        30
  落导入方同目录                             98
  落导入方 helpers/                          11
  无对应 .ta 文件                            26
      ├ net 20 / vm 1                       21     ← S1 补存根
      └ 负例 fixture 4 + consumerlib 1        5     ← 保持/改树名
```

- 同一文件、同一棵树，仅 shell cwd 不同就 `module not found`：见 §2 的实测。
- `test/consumer/vendor/.../lib/` 只有 `consumerlib.ta` + `json.ta` 两个文件，
  而 `main.ta:19` 还 `import str` —— **旧设计靠双根 fallthrough 才活着**，
  所以"vendor 是完整闭包"在无 manifest 前提下不可判定（本设计因此不要求它，
  也不允许它：单根 + 显式链接）。
- 自指符号链接会打断递归遍历：Python `glob('**/*.ta', recursive=True)` 在
  `vendor/tinyactor -> ../..` 下报 `File name too long`。任何工具（排版、
  打包、统计）都必须 `os.walk(followlinks=False)` 并跳过 `vendor/`。

## 12. 否决史（在旧契约基础上新增）

旧契约已否决的（registry、相对导入 `./x`、manifest 映射进语言、`self:`、
vendor 嵌套、cwd 当基准、短名全局 alias、`core:`）继续否决。新增：

- **裸名 → 标准库** ✗ —— 三种东西共用一个命名空间（同目录文件 / `lib/*.ta` /
  C 模块），于是必须"先试文件再查表"，`import util` 的含义取决于导入方旁边
  恰好有什么。名字的职责只能有一个。
- **双根 fallthrough / 项目树覆盖安装树** ✗ —— 实测它让**最常见**的名字
  （335 条标准库）走"两候选 + 顺序优先"，最罕见的第三方树恒一候选。顺序
  优先写在解析器里就是语言规则，而根的取值来自 env/cwd 就是工具约定 ⇒
  同一份源码的模块集合由"你怎么调工具"决定。
- **`tree == 发行树` 时兜底安装根** ✗ —— 需要一条字符串常量（今天的
  `CANON_IDENTITY`，`driver.ta:685`）；fork 改名后恒假、且编译器不会报
  "你的发行树名错了"= cora 的 `(package "x")` 双真相换了个字段。
- **`std:` 保留前缀** ✗ —— 唯一收益是"没 vendor 也能用标准库"，代价是一条
  保留规则 + 一个不能被子树替代的名字。改由 `tinyactor init` 的一条普通链接
  承担：链接就是树，`ls vendor/` 可见，fork 换链接或换目录即可。宁可要
  "一条可见的链接"，不要"一条不可见的保留字"。
- **树名必须含点** ✗ —— 域结构的用途是无注册表时保证全局唯一；本设计里
  唯一性由 `vendor/` 下目录名唯一性直接提供。含点的树名照样合法
  （`example.org/hello`），只是不强制。删掉这条规则连带删掉
  `first_seg_has_dot` 与其负例测试。
- **`<根>` 取 `$PWD` / 环境变量** ✗ —— §2 的实测：同一个入口文件换 shell
  cwd 就失败。根必须能从**入口文件位置**推出来。
- **`./x` 显式相对（Janet/Odin 的形态）** ✗（既有结论，本轮补强）——
  今天它**已经在跑**（实测 `import "./b"` 打印 `HELLO`，见 §1.4），只是没有
  人写：它是同一个文件的第二个名字，与 `canon_of_path` 互相喂养。
  `jpm_tree/lib` 的扁平安装证明相对路径与"拍平 vendor"天然冲突：依赖被复制
  到 `lib/` 顶层后 `./x` 全断，Janet 生态只能靠 `<pkg>/init.janet` 的命名
  约定自救。相对引用的正确用途是"同目录小工具"，而本设计的同目录引用由
  树名覆盖，边界更清楚。

## 13. 待拍板 / 待实测

**要拍（1 条）**：§2.4 / §12「`std:` 不做」意味着**没有 vendor 的目录不能编译**，
`tinyactor init` 成为硬前置（含 10 处 heredoc fixture 与 `/tmp` 脚本要补
vendor）。备选是加一个显式 `--vendor <dir>`（等同 C 的 `-I`）。草案默认不加。

**待实测（3 条，都会影响 S1/S4 的边界）**：

1. `timer`：`lib/timer.ta:42-44` 的 `external fn` 与 `lib/timer.c:296` 的注册
   是否重复绑定；`import "…:lib/timer"` 从 no-op 变真加载后行为是否变。
2. `vm`：`lib/vm.ta` 存根 + 删掉 typecheck 宽松路径后，`test/basic` 与自举
   是否全绿（`vm` 是**急切**注册而非懒 dlopen，与 os/file 的懒路径不同）。
3. `cov`：保留裸名内部协议的代价（`api.c:507-516` 记录过"注册表里出现该名
   会把懒加载短路成 unknown function"的坑），或直接下沉到 VM 行表。

## 14. 测试锚点（改造后）

- 正例：`test/basic/identity-import.ta`（全名 + 默认别名）、
  `test/module/identity-import-alias.ta`（`as` + 别名纪律 + mangle 分支）、
  `test/module/vendor-identity-import.ta`（第三方树 `example.org/hello`）、
  **新增** `test/module/single-root.ta`（同名不同树互不影响）、
  **新增** `test/module/no-vendor-errors.ta`（无 vendor/ ⇒ 报错 + 提示）。
- 消费项目（#368 重造）：`test/consumer/`（`vendor/tinyactor -> ../../..` +
  自己一棵 `example.org/consumer` 树），断言在 `test/run_cli_tests.sh`。
- 负例：`identity-missing-module-errors.ta`（not found 按写出名）、
  `import-empty-segment-module-errors.ta`、`import-dotdot-segment-module-errors.ta`、
  **新增** `import-bare-name-errors.ta`（裸名 `import str` 必须被拒绝）、
  **新增** `import-relative-errors.ta`（`import "./x"` 必须被拒绝）。
- 每次 `make bootstrap` 自身即覆盖"编译器源码用同一套规则解析自己"这条
  主路径（`lib/bootstrap/*.ta` 全部走 `tinyactor:` 全名）。