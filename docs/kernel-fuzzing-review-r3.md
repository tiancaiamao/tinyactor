# TinyActor kernel-fuzzing 设计文档评审（R3，零上下文可读性测试）

评审对象：docs/kernel-fuzzing-design.md（v3 草案，689 行）
评审标准：读完即可直接动手实现；只有"不解决就无法开工"的才标阻塞。
说明：以下所有条目均在**未查看仓库源码**的状态下、仅凭文档本身写出；
文末附核实结果（对 [存疑] 逐项验证后的补充结论）。

---

## 一、阻塞（不解决无法开工）

### B-1 [阻塞] §5.0 / §5.2-T8 / §5.3 / §6.2 —— ADT 构造器与模式的语法完全没有给出
子集表裁定 "ADT + match ✅"，但全文只给了 `type T = ...` 一个占位符和 list 模式
`[h, ..t]`。缺失的具体内容：
- ADT 声明的完整语法（`type T = ...` 省略号里是什么）；
- 构造器的调用/构造值语法（`Some(3)`？`Some 3`？裸 symbol？）；
- 构造器模式在 match 里如何书写（含字段解构）；
- 这些构造在 s-expr dump 中如何编码。
受影响的实现面：golden interp 的 match 求值、T8 的"构造器两两不同"判据、L2 负例的
"构造器字段类型错配"变异——三处都动不了。最小完整示例程序也没有覆盖 ADT。
**期望补充**：一个含自定义 ADT 声明 + 构造 + match（带字段构造器与守卫）的可编译
完整样例，并标注 spec 对应章节。

### B-2 [阻塞] §7.3 —— spawn 无法确定能否向被 spawn 的函数传参
多重集 harness 需要 `worker_i(collector_pid, i, m)` 与 `collector(n_total)`，
但文档给出的语法指针只有 `spawn('fn_name)` / `spawn(fn{..})` 两种形态，均无参数传递
方式。整个 DELIV-8 harness 无法写下第一行有效代码。连带问题：消息载荷是什么类型的值
（int？pair？deep copy 支持哪些类型）——"<wid> <seq>" 需要打包两个整数进一条消息。
**期望补充**：spec《Actor 模型》章的确切 spawn 签名（含传参语法）、send 载荷类型约束、
以及一个两进程 ping-pong 最小样例。

---

## 二、矛盾（文档内部前后不一致）

### C-1 [矛盾] §4 vs §5.1.2 vs §5.1.3 —— ASan 默认 exit code=1 击穿死亡协议
§4 明确 runner 跑在 tavm_asan 底座上；ASan 出错时默认 `exitcode=1`。而协议规定
run exit==1 一律合成 `DIVZERO:<n>` 行，§5.1.2 又断言 panic 类结局 "exit ≠0 且非 1"。
三者合起来：VM 内存 bug 触发 ASan 时会被静默降级成 divzero 并合成一条假协议行，
污染比对且错误分类。文档没有任何 ASAN_OPTIONS 约定或 stderr 判别兜底。
**期望补充**：钉死 sanitizer 运行的 exitcode 约定（如 `ASAN_OPTIONS=exitcode=42`），
或在 norm_tavm 中加入 stderr 特征判别分支。

### C-2 [矛盾] §5.5 / DELIV-4 —— reduce.sh 是 bash，却要求做子树级操作
归约策略第 2 条"表达式子树替换为字面量"需要解析 TA 源码的树结构——bash 没有 parser；
若改为操作 gen 树域，失败样本落盘的只有源码文本和 sha（单向），无法回溯到生成参数。
策略 1（整行删除）和策略 3（token 删减）勉强可在文本域做，策略 2 在 bash 里不可实现。
另外 DEC-6 说工具链统一 Scheme、"make/bash 只做胶水"，DELIV-4 却是纯 `.sh` 交付物。
**期望补充**：明确 reduce 的输入输出契约与实现语言（如 Guile 实现、sh 仅入口），
或删除子树替换策略并说明替代手段。

### C-3 [矛盾] §5.3 —— 字符串 dump 编码两处表述相反
s-expr 编码表写 `string → "hi"（带引号，转义解码）`，同节后文却写"dump 时重新编码
转义（`"`→`\"`、`\`→`\\`、控制字节→`\xNN`）"。解码还是编码？表是快照防漂移的对接点，
两种读法产出不同的 sexp 文本。以后文为准的话表的措辞必须修正。

### C-4 [矛盾] §1.1 vs §4 —— 快速环时间预算自相矛盾
§4 runner 卡：每程序 build+run 至少 4 次（E₀+3 变体），单次超时上限 5s；
§9 快速环要 <5min 跑 500 程序。最坏情况 500×4×5s=10000s；即使正常路径，
每次 `tinyactor build` 要在 tavm 上跑完 tokenizer/parser/typecheck/codegen 全链，
单次成本未估，500×4 次 build 是否能压进 5 分钟没有任何论证。
**期望补充**：单次 build/run 实测耗时数据 + 快速环预算表，或调低 fast 规模。

### C-5 [矛盾·轻微] §1.2 —— 测试套件计数对不上
"测试套件 ≥8 类全绿：basic/gc/actor/module/compiler/bootstrap/example 七类 +
test-cli + test/crash"——列举 7 类却说 ≥8 类，加上后两项共 9 个目标，口径混乱。

---

## 三、歧义（同一表述有多种合理解读）

### A-1 [歧义·高] §5.0 / §5.1.1 —— 分号是分隔符还是终结符；块尾无表达式的块的值
最小示例中 main 最后一条语句不带分号，而骨架写 `print(<expr_1>);`。gen 的 render 和
golden 的环境模型都需要精确规则：`let x=1;` 之后直接 `}` 合法吗（块值是 nil？unit？）？
末条语句带分号合法吗？函数体只有一条表达式时是否等价于返回它？文档只说"块内分号序列"。
第一次跑通样例会撞上，但文档目标是零上下文可实现，此处无法从文本唯一推导。
**期望补充**：块文法的形式化（stmt 分号序列 + 可选尾 expr）及空块/无值块的语义。

### A-2 [歧义·高] §10 DELIV-2#1 —— 节点表求差的定义照字面不可满足
"收集所有 list 节点的 car（symbol 者）与冻结节点表求差 = 空"。按 §5.3 编码，
每个函数调用节点 `(f a b)` 的 car 都是任意用户符号，照字面这个差集对任何含函数调用的
程序都非空。意图应是"出现冻结节点表之外的特殊形节点 → 非空"，但方向（A−B 还是 B−A）、
过滤规则（是否排除应用头符号）都没写。
**期望补充**：改写成可执行的判定谓词，如"遍历中遇到 car∈特殊形集合但 ∉ 冻结表 → fail"。

### A-3 [歧义] §5.4 —— unexpected-divzero 类别的触发条件未定义
gen 允许运行期除零（除数可以是计算出的 0，边界注入还刻意包含 0），此时 divzero 是
合法结局、compare 能对齐、不算 finding。那什么情况下才归入 unexpected-divzero？
该类别在封闭枚举里存在却没有触发条件描述。
**期望补充**：明确定义（例如：divzero 发生但 gen 曾证明除数非零，或各变体死因不一致时）。

### A-4 [歧义] §5.3 / §10 DELIV-1 —— `vm.get_arg(0)` 的索引基不明
惯例上 argv[0] 是脚本自身路径。`tinyactor run ast-dump.ta target.ta` 场景下目标路径
到底是 get_arg(0) 还是 get_arg(1)？两种读法都通顺，写错只是运行一次报错的事，
但接口契约应当写死。
**期望补充**：注明 get_arg(0) 返回的是脚本路径还是首个用户参数，附一行实测样例。

### A-5 [歧义] §5.3 —— match 臂编码 `(pat [guard] body)` 的方括号性质
方括号是 sexp 文本里的字面 token，还是元记法表示"可选"？guard 缺省时臂是二元列表
`(pat body)` 还是三元 `(pat nil body)`？Guile reader 对 `[...]` 有自己的读法，
这直接影响 interp.scm 的臂解析代码。
**期望补充**：给出带 guard 与不带 guard 各一行的真实 dump 输出片段。

### A-6 [歧义] §5.4 —— metamorphic 主断言的比较拓扑
"assert compare 全体一致"：是 E₀ 与各 Eₖ 星形比较，还是四结局全对全？变体之间也要比吗？
（锚点断言明确只作用于 src₀/E₀——这是有意省略还是遗漏，也值得一句话说明。）
**期望补充**：一句伪代码级别的比较循环。

### A-7 [歧义] §9 —— "morph 500 程序"与 seed 配比的口径
500 是基础程序数（×4 执行）还是执行单元数？"固定 seed 基准集 + 滚动新 seed"的比例、
基准集大小、滚动部分每日新增多少——都没有数字。
**期望补充**：具体配比（如 固定300+滚动200）与计数口径。

### A-8 [歧义] §10 DELIV-3#1 —— 验收"全绿"与 skip 通道的关系
runner 定义了 |ts|<3 时的 skip 路径且"不计入验收分母"；验收场景 1 却说"100 基础程序 ×
各恰 3 变体…全绿"。出现 skip 时验收算过还是不过？
**期望补充**：明确验收是否允许 skip 及上限。

---

## 四、缺失（实现必然要问、文档没回答）

### M-1 [缺失·高] §5.2-T4 —— 常量折叠遇除零字面量的行为未定义
T4 把 `<lit op lit>` 折叠为值；当 op 为 `/` 或 `%` 且右操作数为字面 0 时，
原式走死亡协议、折叠会产生一个"值"，直接破坏等价性。必须规定：除零/模零组合跳过折叠。
文档完全没提。（gen 还会主动产出 `1/0` 形态的卡，T4 必然撞上。）
**期望补充**：T4 正确性条件加一条"op∈{/,%} 且右操作数=0 时禁用"。

### M-2 [缺失·高] §5.4 / §9 —— gen 确定性所依赖的 PRNG 未钉死
"gen 必须对 seed 确定（同 seed 同程序）"是去重与复现的基石，但没说随机数算法。
Guile 内建 random 跨版本不保证稳定。**期望补充**：指定自实现 counter-based PRNG
（如基于 sha256 的确定性流）或锁定 Guile 版本。

### M-3 [缺失·高] §5.4 runner 调用卡 —— 直接调用 tavm 二进制的细节整体缺位
"<asan tavm 二进制> <artifact>"：artifact 扩展名/格式、裸 tavm 是否接受 .tabc 直跑、
二进制路径常量放哪、tinyactor/tavm/guile 三个可执行文件的发现机制——文档自己标注
"实现首日钉死"，但这正是 runner 的第一行代码。
**期望补充**：至少给出一组已验证可行的命令行样例。

### M-4 [缺失] §5.0 —— 运算符全集未列出
子集表写 "`< <= == != ...`"——省略号里有什么？T7 需要 `>` `>=` 存在；`==` 对
pair/list 是深比较还是引用比较（golden 必须逐字节复刻该语义）；字符串 `==` 的语义。
**期望补充**：完整运算符表（或精确到 spec 章节号的指针）+ `==` 在复合值上的语义实测结论。

### M-5 [缺失] §5.0 —— bool/nil 字面量不在子集表
T5 用到 `if true {...}`，cons 链终止需要 nil，打印事实也列了 true/false/nil——
但子集表没有这两行。gen 能否产出 `let b = (x < y); if b {..} else {..}`？
**期望补充**：补 bool/nil 字面量行及其类型规则。

### M-6 [缺失] §5.4 / §10 —— batch 内 seed 派生方案
"seed=42 batch=100"：seed 序列是 42+i、sha(42,i) 还是链式？不同派生影响回归集稳定性。
**期望补充**：公式。

### M-7 [缺失] §9 —— 滚动 seed 公式细节
`seed = sha256(git_sha ∪ 日期 ∪ 计数器)`：∪ 的拼接格式（分隔符、字段顺序、日期格式）、
计数器的粒度（全局递增？每 ring？谁维护、存哪）均未定义。"可复现"承诺悬空。
**期望补充**：一段可直接抄的伪代码。

### M-8 [缺失] §5.0 / §5.4 —— gen 程序规模分布未定义
表达式深度、语句数、顶层函数数的上下界。影响：超时阈值的有效性、归约起点规模、
覆盖率论证、fast 环预算。**期望补充**：分布参数表。

### M-9 [缺失] §5.2 —— 定向探测混入生成分布的比例
"混入生成分布，非变换"——边界值/遮蔽链/深尾递归各占多少？
**期望补充**：比例或独立 batch 的划分方式。

### M-10 [缺失] §9 / §4 —— gitignore 目录承载"固化/冻结"产物的张力
build/kernfuff 整个目录 gitignored，但 AST 快照要"冻结存档"、typecheck 负例要
"固化快照"、expected-fail 清单要"固化"。跨机器/跨 clone 的 CI 上这些文件从哪来——
入库还是每次再生成？两者语义冲突。
**期望补充**：逐产物标注入库 or 再生成（及再生成的确定性依据）。

### M-11 [缺失] §10 —— 一半交付物没有验收场景
§10 只覆盖 DELIV-1/2/3/5/8；DELIV-4（reduce 除 DELIV-3#4 外）、6、7、9、10、11
没有对应验收条目。**期望补充**：补齐或显式声明哪些以其他场景代验。

### M-12 [缺失] §5.5 —— crash 类"stderr 特征串相同"的归一化规则
panic 文本通常含地址/行号，逐次运行可能不同。特征提取规则（正则？前 N 字节？去地址化？）
未定义，否则 reduce 复现判据不稳定。
**期望补充**：特征串的定义与归一化算法。

### M-13 [缺失] §7.4 / §9 —— 目标平台与 TSan 可行性未声明
项目路径显示 macOS；TSan 在 macOS（尤其 arm64）支持长期受限。TSan 战线与 nightly
接线依赖平台结论，文档通篇未提 OS/CI runner。
**期望补充**：声明目标平台矩阵及 TSan 在其上的验证状态。

### M-14 [缺失] §5.3 —— dump 输出中 `'begin` 引号形式与 Guile reader 的交互
dump 打印 `('let ...)` 等；Guile 读入 `'let` 得到 `(quote let)` 而非符号 let。
interp 需要一个规范化约定（strip quote / 自写 reader），文档未提。
**期望补充**：一句"interp 读入后先做 X 归一"。

### M-15 [缺失·轻] §5.0 —— main 的签名约束与混合注解的一般规则
main 是否必须有/无返回类型注解；`fn twice(g, y: int)` 这种部分参数带注解是否为一般
合法形态（目前只有孤例）。**期望补充**：注解省略规则的成文表述。

### M-16 [缺失·轻] §4 / §5.3 —— *.sexp 文件命名与落盘责任
语料路径到快照文件名的映射规则（目录斜杠处理）、ast-dump stdout 由谁重定向落盘。
**期望补充**：命名模板一行。

---

## 五、存疑（怀疑与真实代码/spec 不符——以下为纯阅读阶段假设，核实结果见附录）

### Q-1 §1.1 —— 行号引用群
`val.c:58,147`、`vm.c:123-160`（print 算法）、`vm.c:296-298`（divzero）、
`tavm.c:136-141`（exit/fflush/fsync）、`parser.ta:894-969`（负号折叠）、
`driver.ta:573`（file.read）、`lib/driver.ta:364-368`（typecheck 流水线）、
`parser-ast.ta:255`（`((f 1) 2 3)`）、`Makefile:24-35`（TSAN）。
另：文件清单表里 C VM 只有 vm/scheduler/gc/val 四个文件，`tavm.c` 从未出现在结构表中，
却在关键事实里被引用——它在哪一层？

### Q-2 §1.1 —— 词法事实"负字面量仅在 `use x <- -5` 粘连位置由 tokenizer 折叠"
表述古怪：use/bind 已被 v0 排除，这条"唯一折叠位置"却建立在被排除语法上；
且"粘连位置"定义不明。怀疑对 parser.ta:894-969 的解读有误。

### Q-3 §1.1 —— 死因枚举拼写 `cdreerr`
car/cdr 错误分别叫 `carerr`/`cdreerr`——后者多了一个 d，怀疑是 `cdrerr` 的笔误
（若是笔误，golden/分类学文档会跟着错）。

### Q-4 §1.1 —— 内建名群
`vm.get_arg(0)`、`file.read(path)`、`str.concat`、`str.from_int` 是否存在且名字准确。

### Q-5 §9 —— CLI 事实群
`tinyactor fmt` 子命令存在且就地改写；`tinyactor build -o <path>` 支持。

### Q-6 §5.0/§5.2 —— 类型系统事实群
大写 `Int` 被 typecheck reject；箭头类型注解不支持；`fn callf(g){ g(41) }`
无注解参数可编译；`fn(){..}` 与 `fn{..}` 两形态且 AST 同构。

### Q-7 §1.1/§5.2 —— VM 事实群
TAG_CLOSURE NaN-boxing 标签存在；OP_TAIL_CALL 存在；match desugar 为嵌套 if+`=`；
穷尽性 warning 文案确为 `non-exhaustive match`；typecheck reject 文案确为
`typecheck: N type error(s) found`。

### Q-8 §1.2 / §3-DEC-2 —— "既有 mutation fuzz"是否存在
DEC-2 说"tokenizer/parser 由既有 mutation fuzz 负责"，但 §1.2 资产清单里只有
fuzz-regression.ta（历史 case 列表）与 compare-parsers.ta，没有任何活跃 mutation
fuzzer。怀疑"既有"指的只是一份回归 case 清单——那么 DEC-2 的分工前提不成立。

### Q-9 §1.1/§1.2/§4 —— 资产存在性群
docs/ta-language-spec.md 存在且章节名与所引一致（《ADT·声明语法》《Actor 模型》
《没有可变状态》《变量绑定》等）；AGENTS.md 存在且含分层原则/bootstrap 成本/
parser-ast 同步义务；docs/kernel-fuzzing-review-r1.md / r2.md 存档存在；
test/basic/closure.ta、test/basic/string-escapes.ta、test/compiler/fuzz-regression.ta、
compare-parsers.ta、run_crash_tests.sh、make test-cli、make bootstrap-selfhost、
bench/serve_bench.go 存在；tools/ 目录确实为空；tavm --help 确无栈深选项。

### Q-10 §1.1 —— 各文件行数表（638/1606/3113/1921/696/1123/674/277/289）

### Q-11 §5.3 —— ast-dump 的 TA 工具自身可行性
TA 程序内 import tokenizer/parser 后能否拿到"parse 整个文件得到 cons 树"的单入口
（driver.ta 的哪条路径），以及 import 语法本身长什么样——文档给了用途没用给入口签名。

---

## 附录：核实结果（完成纯阅读评审后，对照仓库 /Users/genius/project/tinyactor 验证）

（见下方"六、核实附录"。）

VERDICT: NOT-READY（阻塞数=2）