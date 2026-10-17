# 模块与 import 契约（E2）

本文是 import 解析的**契约文档**：语言层面关于模块名字与解析的全部规则。
实现锚点：`lib/bootstrap/driver.ta` 的 `find_module_path`（宿主编译器）与
`tools/expand_imports.py` 的 `module_candidates`（corpus 文本展开器）——两者
同语义，改一处必须同步另一处。展开器只服务仓库内 corpus，把**仓库根**当
项目根，正是根表（下节）未设 `TA_PROJECT_ROOT` 时的默认情形。

## 契约（五条，全部）

1. **identity 形态**：`import "<identity>:<path>"` 的唯一解析机制是机械拼接
   `<根>/<identity>/<path>.ta`（根表见「解析基准：vendor 根表」）。没有第二条
   路径，没有搜索、没有注册表。
2. **裸名糖**：`"aaa"` ≡ `"github.com/tiancaiamao/tinyactor:lib/aaa"`；斜杠
   子路径同理（`"bootstrap/tokenizer"` ≡ `…:lib/bootstrap/tokenizer`）。
   标准库就是普通包：发行版预装其 vendor 目录（仓库内是符号链接
   `vendor/github.com/tiancaiamao/tinyactor → 仓库根`）。fork = 覆盖 vendor
   目录，**没有任何保留规则**。消费项目自带 vendor 树时（issue #368）裸名
   先落在**项目**的 `…/lib/` 下，没 vendor 的才落回安装树——发行版只是项目
   树里的一个包，不是一条特殊的解析规则。
3. **`as n` 文件本地短名**：语法现成，语义是文件内别名纪律（一个文件里每个
   模块只有一个可用拼写，写真名报错）。
4. **卫生**：段禁空、禁 `..`；identity 首段必须含点（域结构保证全局唯一，
   不需要注册表）。违规响亮拒绝：`error: invalid module name: <name>`。
5. **规范名收敛（一个文件 = 一个单元）**：unit 键取**解析后文件**的规范名
   （`canon_of_path`：根表的每个根下 `…/lib/` 之内 = 相对 `lib/` 的拼写，如
   `lib/bootstrap/kl.ta → bootstrap/kl`——每个根都要认，否则同一份标准库
   换成命中项目树就漏出第二套规范名；`…/lib/` 之外 = 写出名），与书写拼写
   无关。
   裸名糖 / 子路径糖 / identity 全名命中同一文件时静默合流为同一单元——
   糖只是缩写，限定前缀因此唯一，别名改写天然一致，混拼写不是错误。
   正例：`test/module/mixed-spelling-converge.ta`（单文件两拼写共存 +
   跨单元先 identity 后裸名，与加载顺序无关）。

**语言不知道「版本」**：冲突解决 / fetch / lock 是用户与工具的事。冲突不是
被解决而是被移除——两个 identity 天然不撞；同一个 identity 想要两个版本，
复制目录即可，语言没有「版本」这个概念可供表达。

## 解析基准：vendor 根表（driver.ta `vendor_roots`）

契约里的 `vendor/…` 不是写死的 cwd 相对串，而是**根**下的拼接：模块树属于
调用方项目，不属于编译器进程。根按优先级排列：

1. `$TA_PROJECT_ROOT/vendor/` —— 调用方项目根（`tinyactor` CLI 从 `$PWD`
   传入）。消费项目自带的 vendor 树因此被看见，且**项目树整个压过安装树**。
2. `vendor/`（cwd 相对）—— 编译半程的 cwd 恒为安装树（CLI 的
   `cd "$SCRIPT_DIR"`；编译期 `dlopen("lib/<mod>.dylib")` 是 cwd 相对的，
   换 CWD 会让编译器自己的 stdlib dylib 不可达），所以这一项 = 发行版预装
   的 vendor 树，是兜底：没 vendor 的裸名仍解析到今天那份标准库。

根是外层循环、形态是内层：项目树里**任何**一份都盖过安装树的同名者（不是
逐形态交替）。未设 / 空 `TA_PROJECT_ROOT`（`make bootstrap`、直接
`tavm lib/bootstrap.tabc …`、corpus 文本展开器）时只剩第 2 项 —— 解析结果
与引入根表之前逐字节一致（默认等价性）。

## 解析顺序（find_module_path）

- identity 形态（含 `:`）：只走 `<根>/<identity>/<path>.ta`（根表顺序），找
  不到即 `error: module not found: <name>`（按写出名报，不补 `.ta`——identity
  是身份不是路径）。
- 裸名/子路径（标准库糖）：
  1. `{导入方所在目录}/{name}.ta`（项目内模块/本地覆盖优先）
  2. `{导入方所在目录}/helpers/{name}.ta`（测试/辅助模块约定）
  3. 根表 × `<identity>/lib/{name}.ta`（标准库；项目树先于安装树）
  4. 根表 × `<identity>/lib/bootstrap/{name}.ta`
     （编译器自身源码；compiler 测试套与内核自举仍按名导入 tokenizer/parser）

导入图按解析后的路径去重（菱形依赖只加载一次），环显式检测
（`error: circular import: a -> b -> a`）。

宿主 C 模块（net/timer/cov 已下沉为 dylib-backed 词条，num/vm 仍静态注册）按裸名在进解析器之前短路，import 是
no-op；它们没有 vendor 落点，也不需要。

## 显示名与内部名（两条边界）

- **显示名** = import 写出的字符串（`github.com/alice/aaa:lib/json`）。
  错误信息用写出原名；导入图 dep 匹配、sig cache 语义键也用显示名
  （两侧拼写不一致时只导致 cache miss→重验，安全方向）。解析键/去重/
  eff_key 不接触内部名；unit 键 = 规范名（契约第 5 条）。
- **内部名** = lowered 限定符号的前缀（`qual_prefix(规范名)`，纯函数）：
  - 规范名不含 `:` `.` `/`（可写作源码引用头）→ 原样。既有语料全部走此
    分支，lowered 输出零变化（程序产物逐字节门成立的原因）。
  - 否则（identity / 子路径 / 含点的规范名）→ 压缩为
    「末段截 24 字符 + `~` + 名字哈希十进制」，
    如 `github.com/alice/aaa:lib/json` → `json~1203948576`、
    `bootstrap/kl` → `kl~<h>`。**`/` 不豁免 mangle（评审修订）**：曾有
    提议对 `/` 保留原样，但存在需要无 `/` 名字的场合——emit-kl-dir 的
    分块文件名、kl 标签（`seq_tagged`）都直接用前缀拼名字，含 `/` 会
    落进子目录/切错段，故子路径糖同样 mangle。长度有界（kl 符号上限
    255），无冒号无点；选首点切头无关的形态是因为别名重写
    （`alias_rewrite_sym`）按第一个 `.` 切头，含点前缀会功能性切错。
  - 哈希函数 = `modsig.str_hash`，如实描述：初值 2166136261，逐步
    `h' = ((h mod 2²⁴) × 16777619 + c) mod 2³²`（TA int 48 位，每步
    归一化进 [0, 2³²) 保证键稳定）。它**不是 FNV-1a**——没有 XOR 步，
    且先截 24 位再乘；此前文档写「FNV 哈希」不准确。碰撞分析不变：
    head24+32bit 同碰撞概率 ≤2⁻³²/对；不靠概率兜底——两个不同显示名
    压出同一前缀时 typecheck 按既有导出表路径响亮红，绝不静默。
- **kl 文本序列化**写出 mangled 内部符号；reader 回读按同一符号表拼回，
  `write(read(T)) == T`（G-A 文本幂等）与树等价在 mangled 名上同样成立，
  无需 kl 层知道任何 mangling 规则——它只是搬运符号。

**自举种子约束**：lib/bootstrap 内部的子路径 import 必须写成显式
`as <末段>`（如 `import "bootstrap/kl" as kl`）。`make bootstrap` 的第一遍
用上一次的 bootstrap.tabc 当种子编译新源码，种子只认识「显式 as」的别名
对；无 `as` 的默认别名（末段）是新契约语义，种子编译第一遍就会 typecheck
红。显式拼写与默认别名语义一致，收敛（fixed point）后逐字节稳定。

## core: 的退役

`core:<name>` 前缀已删。它原本表达的「钉在核心树、不可被影蔽」由两个更
简单的机制承接：identity 形态天生钉在 `<根>/<identity>/` 下（机械拼接，
无搜索路径自然不可影蔽）；标准库裸名糖落点是发行版自己的 vendor 目录，
fork 通过覆盖该目录实现，不存在需要「保留前缀」保护的东西。原 `core:` 测试
改写为 identity 形态正/负例（`test/basic/identity-import.ta`、
`test/module/identity-import-alias.ta`、`test/module/identity-missing-module-errors.ta`）。

## 否决史（防后人重议）

- **registry / 中央注册表** ✗ —— 域结构 identity 已保证全局唯一，注册表
  是多余的中心化组件。
- **相对导入 `./x`** ✗ —— gleam/go 全部绝对 identity；相对路径让模块身份
  依赖文件位置，移动即坏。
- **manifest 映射进语言** ✗ —— 名字→目录的映射层是包管理器的事，塞进语言
  就是第二个解析机制。
- **`self:` 保留字** ✗ —— 又一个保留前缀；「仓库即发行版」的 vendor 符号
  链接让仓库引用自己与引用别人走同一条机械拼接。
- **vendor 嵌套** ✗ —— vendor 里再 vendor，解析规则复杂化，无真实需求。
  （消费项目自带 vendor 树不是嵌套：根表只有「项目根 → 安装树」两根，
  项目树里那份自己不会再往安装树里找。issue #368）
- **解析基准 = 编译器进程 cwd**（现状，issue #368）✗ —— 把模块树绑在编译器
  进程上：消费项目 `cd $PWD && tinyactor build` 时自己 vendor 的树看不见，
  而模块树属于**调用方项目**，不属于编译器进程。修法是让基准显式化（根表
  + `TA_PROJECT_ROOT`），不是让 cwd 冒充基准。
- **编译半程换 CWD 到调用方项目根** ✗ —— 「换 cwd = 换基准」看着更简单，
  但编译期 `dlopen("lib/<mod>.dylib")` 是 cwd 相对的（用户的项目里没有
  编译器自己的 `lib/sexp.dylib`），要么连 C 层模块查找一起改（宿主 exe 目录
  回退只覆盖一部分），要么把 dylib 解析也搬进根表——为了一个根把自己
  拆成两套。根表让 CWD 语义原样不动。
- **短名全局 alias** ✗ —— 无注册表时短名不可全局唯一；`as` 就是文件本地
  短名的全部。
- **core: 保留** ✗ —— 与 `vendor/core` 撞，且其功能被 identity + vendor 糖
  完全覆盖（见上节）。
- **mangling 备选**（内部名方案内）：全哈希 ✗（可写名也变，产物门必破）；
  不 mangle ✗（identity 含点，别名重写按首点切头会切错；`/` 原样保留 ✗
  ——kl 分块文件名/标签需要无 `/` 前缀）；注册表映射 ✗
  （中心状态）；纯截断 ✗（截断碰撞概率远高于截断+哈希）。
- **混拼写报错** ✗（评审修复轮否决）—— 把「同一文件两种拼写」当错误会在
  菱形依赖里把裁决交给加载顺序，非确定性；规范名（契约第 5 条）让糖只是
  缩写，收敛是纯函数行为，无需报错。
- **first-wins 单元注册表** ✗（评审修复轮否决）—— 同样把键绑定到首次
  加载的拼写；`canon_of_path` 用解析路径派生规范名，与顺序无关且确定性。

## 测试锚点

- 正例：`test/basic/identity-import.ta`（canonical identity + 默认别名）、
  `test/module/identity-import-alias.ta`（`as` + 别名纪律 + mangle 分支）、
  `test/module/mixed-spelling-converge.ta`（规范名收敛：identity/裸名/
  `as` 混拼写，单文件 + 跨单元；子路径糖的 mangle 分支由每次自举
  `make bootstrap` 覆盖——driver.ta 自身全部走 `import "bootstrap/*" as *`）、
  `test/module/vendor-identity-import.ta`（第三方 fixture，
  `vendor/example.org/hello/lib/greet.ta`）。
- 消费项目 vendor 树（issue #368）：fixture `test/consumer/`（自指 identity
  符号链接 + 项目自带的 stdlib 裸名模块 + 影蔽安装树同名者的项目副本），
  断言在 `test/run_cli_tests.sh`（`tinyactor build` / `run` 从项目根调用）。
- 负例：`test/module/identity-missing-module-errors.ta`（not found 按写出名）、
  `test/module/import-empty-segment-module-errors.ta`（`a//b`）、
  `test/module/import-dotdot-segment-module-errors.ta`（`a/../b`）、
  `test/module/import-identity-no-dot-module-errors.ta`（首段无点）。