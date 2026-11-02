# 模块与 import 契约（E2）

**一句话：`import` 只有一种写法 `<树名>:<树内路径>`；候选恒一条；写出的名字就是
模块身份。**

```ta
import "example.org/hello:lib/greet"          // → <根>/vendor/example.org/hello/lib/greet.ta
import "example.org/hello:lib/greet" as g     // 文件本地短名：g.greet ...
import "github.com/tiancaiamao/tinyactor:lib/str"   // 工具链标准库也是一棵普通树
```

与本契约之前的实现的差别只有三句：**没有裸名**、**没有相对路径**、**没有第二根**。
其余全部是删：没有候选列表、没有搜索顺序、没有遮蔽、没有「显示名 / 规范名」二分。

## 1. 名字

导入名是一个字符串，形态唯一：`<树名>":"<树内路径>`。

1. **恰好一个 `:`**，它就是树名与路径的唯一分界。分界显式，所以
   `example.org/hello:lib/greet` 的树名不可能被读成 `example.org`——不需要前缀
   搜索，也不需要「首段含点」来消歧。
2. **树名**：一段或多段（`tinyactor` / `example.org/hello`），段之间 `/`；每段
   非空、非 `.`、非 `..`、不含 `:`。**当前实现另外要求首段含点**（无可信注册表
   时靠域结构保证全局唯一）；这条约束是唯一一条与「树名只是 `vendor/` 下的目录
   名」无关的历史遗留，放宽它不影响任何解析规则。
3. **树内路径**：同上段规则，`/` 分隔，**省略 `.ta`**。
4. **卫生违规响亮拒绝**：`error: invalid module name: <name>`。拒绝的情形：
   空名、裸名（无 `:`）、空段（`a//b`）、`.` 段、`..` 段、第二个 `:`、树名首段
   无点。`.` 段必须拒：它会被机械拼成 `<根>/vendor/<树>/./x.ta`，也就是同一个
   文件的**第二个名字**——而名字即身份，一个文件只有名字允许的那一个。
   负例见 `test/module/import-*-module-errors.ta`。

## 2. 根

1. **根 = 从入口源文件所在目录向上，第一个含 `vendor/` 的祖先目录。**
   一次构建算一次。
2. **不读 `$PWD`、不读任何环境变量。** 同一个入口文件从任何 cwd 调用都解析
   到同一棵树（锚在 `test/run_cli_tests.sh`：同一个消费项目分别从项目根和
   `app/` 子目录 build / run，输出逐行比对）。
3. **只有一个根。** 没有家目录兜底、没有安装树兜底、没有 `std:` 保留前缀。
   项目要工具链标准库，就由 `tinyactor init` 在项目里建一条**普通树链接**：

   ```bash
   vendor/github.com/tiancaiamao/tinyactor -> <安装根>   # 相对链接，随安装位置移动仍有效
   ```

   工具链仓库自己用同一条机制指回自己（`vendor/github.com/tiancaiamao/tinyactor
   -> ../../..`）。链接是相对路径，仓库换个位置克隆不需要改。
4. `vendor/` 下**任何目录（含符号链接指向的目录）都是一棵树**，树名就是它在
   `vendor/` 下的相对路径。树内部的 `vendor/` 是死数据，永不参与解析——这就是
   「依赖平铺、vendor 只有一层」的可执行形式。
5. **入口文件有 import 又找不到 `vendor/` 时报错**（§8），**不回退**；没有
   import 的脚本不看根，照常编译。这是本契约唯一的硬前置：要 import 标准库的
   一次性脚本也得先 `tinyactor init`（一条符号链接，或把标准库源码拷进
   `vendor/`）。

## 3. 解析

```
resolve(name, root):
    (tree, path) = split_once(name, ":")              // 卫生检查在此之前完成
    file = root + "/vendor/" + tree + "/" + path + ".ta"
    content = file.read(file)                          // 命中判据：这一读
    if content == null: error(§8)                       // 候选恒这一条
    return (file, content)
```

- **命中判据 = 一次 `file.read`。** 没有 `exists` 探测、没有候选列表、没有顺序、
  没有遮蔽。
- 名字到文件是**纯函数**：`resolve` 不接收「导入方目录」这个参数。
- 入口文件自身不需要被 import，它由 CLI 直接交给编译流程。

## 4. 身份：单元键、去重、环

- **单元键 = 写出的名字。** 唯一形态 ⇒ 唯一拼写 ⇒ 一个文件在给定构建里只有
  一个名字，于是「同一个文件被两种拼写打开」这种情形不存在。
- **去重**：同一名字在导入图里只加载一次。菱形依赖天然合流，因为菱形两侧写的
  是同一个名字。
- **环**：显式检测，`error: circular import: a -> b -> a`。
- 工具后果：`grep -oE '^import "[^:"]+:' --include=*.ta -r . | sort -u` 直接给出
  本项目的依赖树集合，不需要 manifest。反过来，被 vendor 的库源码里写着
  `hello:lib/greet`，它必须落在 `vendor/hello/` 下才能解析——**树名由源码自证**，
  不存在声明文件。

## 5. 别名

- `import "<名字>" as n`：`n` 是**文件本地**短名。
- 无 `as` 时默认别名 = **名字末段**（`…:lib/bootstrap/parser` → `parser`）。
- **一个文件里每个模块只有一个可用拼写**：写了 `as n` 之后，再用真名引用会被
  拒绝（`error: module '<pfx>' is imported under an alias in this file; the real
  name is not usable here ('<spelling>')`）。`<pfx>` 是**末段**——判据比末段，
  与名字怎么拼、是不是标准库无关（`driver.ta` 的 `aliased_real_pfx`）。反之亦
  然。别名纪律是文件内的，不跨文件、不全局。

## 6. 内部名（lowered 限定前缀）

引用头在 lowered 产物里被 mangle 成内部前缀（`qual_prefix`）：名字不含 `: . /`
时原样，否则「末段截 24 字符 + `~` + 名字哈希」。三条约束没变：不能按首点切头、
kl 标签与分块文件名不能带 `/`、长度有界。

- **唯一例外是发行版自己的树**：名字在 `SELF_IDENTITY` 树下的 `lib/` 之内时，
  前缀用 `lib/` 之后的相对名（`str`、`bootstrap/parser`）。原因在 C 侧——C 函数
  名（`str.trim`）与 `typecheck.ta` 的前置承诺都是全局裸名，前缀一旦 mangle，
  `external fn` 声明仍叫 `str.trim`、引用却被改写成 `str~<h>.trim`，两边永远对
  不上。例外只影响内部名，不影响单元键（名字仍是唯一身份）。
- 因为内部前缀由名字派生，**改名就会改产物**：`import "…:lib/str"` 的前缀是
  `str~<h>`，`.tabc` 与冻结快照都随之变。fixed point 门（同一份源码连编两遍
  逐字节一致）不受影响；跨版本比对产物时以当前源码为准重新生成。

## 7. 边界：C 函数不是模块

模块 = 树里的 `.ta` 文件，没有第二种。C 提供的函数用 `external fn` 声明，
`lib/os.ta` / `file.ta` / `str.ta` / `buffer.ta` / `timer.ta` / `net.ta` /
`vm.ta` 都是这个形态。

- 声明是**强约束**：参数写 `Int` 就只接受 `Int`（`net.close("x")` 编译期
  E0001）；返回类型写 `a` 表示「raw 层返回值是联合，箭头表达不了」。
- 声明**不管元数**：元数来自 `typecheck.ta` 里硬编码的 `builtin_arity` 表，所以
  可选参数（`net.read(fd[, n])`、`net.connect(host, port[, timeout_ms])`）表达不
  了——语料一律写出显式实参，让「声明的元数」= 「所有调用点的元数」。
- **唯一例外是 `cov`**：它由 parser 合成（`(cov.line path line)`），全仓库 0 处
  `import cov`，用户写不出来。它是编译器内部协议，不是可导入的模块。

## 8. 错误信息

模块不是找到的、是拼出来的，所以报告把那一拼摊开：

```
error: module not found: example.org/hello:lib/nope
  tried: /proj/vendor/example.org/hello/lib/nope.ta
  root:  /proj   (vendor/ here)
```

没有根时（唯一的「配置型」错误）：

```
error: no vendor/ directory above /tmp/x.ta
  module names resolve as <root>/vendor/<tree>/<path>.ta
  searched upward from: /tmp
  run `tinyactor init` in your project to create one
```

名字违规：`error: invalid module name: <name>`（按写出名报，不补 `.ta`——名字是
身份不是路径）。环：`error: circular import: a -> b -> a`。

## 9. 被 vendor 时语义不变；改名 = 改身份

设库 `hello` 的源码里有 `import "hello:lib/util"` 与
`import "github.com/tiancaiamao/tinyactor:lib/str"`。

- 另一个项目把 `hello` 放进自己的 `vendor/hello/` → 两条 import 文字不变，
  分别落在新根的 `vendor/hello/lib/util.ta` 与
  `vendor/github.com/tiancaiamao/tinyactor/lib/str.ta`（或明确的一条 not found）。
  **没有一条规则依赖「这个文件原来在哪」。**
- 想把 `hello` 改名成 `acme/hello`：只改放它的目录名不够（源码里的 `hello:`
  前缀会失配），必须一起改被 vendor 源码里的前缀。这是**故意的**：改名就是改
  身份，编译器会逐个 not found 地点名，工具可以机械改（`sed` 一次即可）。相对
  路径能省掉这一步，代价是同一份源码在不同位置语义不同——那正是要消灭的东西。
- 同一份源码在 `make bootstrap`、`tinyactor run`、`tavm` 直跑三种宿主下解析
  结果一致：**根只由入口文件位置决定**，与谁在跑、cwd 是什么无关。
- fork 一个库 = 覆盖 `vendor/` 里那棵树；**没有保留规则**，没有「钉子」，也没有
  需要安装器保护的目录。

**语言不知道「版本」。** 冲突解决 / fetch / lock 是用户与工具的事：两个不同的
树名天然不撞；同一个树名想要两个版本，复制目录改成两个名字即可，语言没有
「版本」这个概念可供表达。

## 10. 测试锚点

- 正例：`test/basic/identity-import.ta`（identity 形态 + 默认别名）、
  `test/module/identity-import-alias.ta`（`as` + 别名纪律 + mangle 分支）、
  `test/module/single-root.ta`（一个根、多棵树：同末段的两棵树互不干扰）、
  `test/module/vendor-identity-import.ta`（第三方树 fixture）。
- 消费项目（issue #368）：fixture `test/consumer/` 装三棵树——自指链接
  `vendor/example.org/consumer -> ../..`、工具链标准库链接
  `vendor/github.com/tiancaiamao/tinyactor -> 仓库根`、第三方树
  `example.org/hello` 的项目副本（安装树也有一份，内容不同，单根下命中的必然
  是项目这份）。断言在 `test/run_cli_tests.sh`（`tinyactor build` / `run` 从任意
  cwd 调用）。
- 负例：`test/module/import-bare-name-module-errors.ta`（裸名）、
  `test/module/import-relative-module-errors.ta`（相对路径）、
  `test/module/import-dot-segment-module-errors.ta`（`.` 段）、
  `test/module/import-empty-segment-module-errors.ta`（`a//b`）、
  `test/module/import-dotdot-segment-module-errors.ta`（`a/../b`）、
  `test/module/import-identity-no-dot-module-errors.ta`（树名首段无点）、
  `test/module/identity-missing-module-errors.ta`（not found 按写出名报）。
- import 别名的静态检查：`tools/check_module_imports.py`（只允许
  `<别名>.<成员>` 形态的引用，写全名即报）。