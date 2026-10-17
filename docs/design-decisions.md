# 设计决策记录（PR #15 review 对齐）

> 与用户在 PR #15 review comments 对齐过程中确认的设计决策。
> 每个决策：结论 + 理由 + 影响面。实现前以此为准。

## D1. 外部 C 模块类型声明机制（external fn）

**结论**：独立声明文件 `lib/<mod>.ta`（与 dylib 同名），语法 `external fn` + 可选绑定。

```ta
// lib/mymod.ta —— mymod.dylib 的声明文件（外部模块作者只写这个 + .c）
external fn double(x: Int) -> Int = "mymod" "double"   // 显式绑定（C 侧名不同时）
external fn greet(name: String) -> String              // 缺省 = 模块名.函数名同名
```

- **装载即生效，import 才有类型承诺**：`.tabc` 装载时 `link_unit` 按名解析并自动 dlopen（不 import 也能调用 `mymod.foo`）；但只有 `import` 才把签名注册进 typecheck 环境，不 import 走宽容 dotted-call 路径（可调用、无类型承诺）
- 类型语法**无需扩展**：现有标注语法已覆盖 Int/String/Bool/Nil/Pid/Pair/泛型（'a）/箭头（a -> b）/复合（List(Int)）；`pair` 经 `t_base` 自动得到 t_pair()
- **内置静态模块（str/net/file/vm）留在 typecheck extend 链**：它们是 main.c 静态注册，非 dylib，声明文件机制不适用（用户：标准库在 typecheck 写问题不大）
- **迁移验证案例（一步到位）**：`demo`（建 lib/demo.ta + 测试加 import demo，成为完整模板）+ `http`（建 lib/http.ta + 删 typecheck.ta e60-e62 的 http 行；kv_server/http_server 已 import，回归验证）
- check-modules.sh 的职责被声明文件机制替代 → 用户明确「不需要这种东西」（去留见 D6）

## D2. 布尔逻辑对齐 Gleam

**结论**：外部语法 `&&`/`||` 中缀，AST 保持 `('and a b)` / `('or a b)`。

- TA 源码写 `a && b` / `a || b`；`&&` 优先级高于 `||`（gleam 一致：`a && b || c` = `(a && b) || c`）
- parser 转换：`&&`/`||` → `('and x y)` / `('or x y)`；**codegen 的 compile_and/compile_or 与短路逻辑不动**（7de77f4 保留）
- **砍掉外部 `and`/`or`**（中缀 + `(and ...)` 前缀形式），恢复为普通标识符
- **语义严格 Bool**：操作数 + if 条件 typecheck 必须 unify 到 Bool，非 Bool 编译期报错；and/or 返回 Bool（不再是值返回，0 不是 truthy）
- 配套：logic-and-or.ta 重写为 Bool 语义；新增 if 严格检查 + `&&`/`||` 负测试；tokenizer 支持 `&&`/`||` token
- 影响面已审计：lib/*.ta 内部零 and/or 使用、if 条件全部是 Bool 表达式 → 收紧零破坏

## D3. 模块级编译（B 方案：编译期链接）

**结论**：模块预编译为 `.tabc` 段 + `.sig` 签名表，主程序 import 时**编译期拼接（链接）**进输出，仍是单文件 .tabc。

- 产物放 `lib/<mod>.tabc`（与源码同目录，仿 dylib 约定）；签名走 `modsig.ta` 管的 sig cache（`.build/modules/<name>-<key>.sig`），仓内无入库的 `lib/<mod>.sig`
- **tavm/运行命令零改动**：多单元链接在编译器侧完成（driver/`.kl` 链接），输出仍单文件；早先设想的 VM 侧 `vm_append_module` **已无定义**（仅 `src/api.c:24` 注释残句）
- **非 pub 不导出**：签名表只含 pub 函数（pub 是真正的接口边界）；非 pub 编译期不可见（主程序 typecheck 拦截）
- **命名空间隔离推迟**：符号冲突现阶段未爆发，不做模块前缀系统（光谱原则，不过度设计）
- **附带收益**：`math.abs` 等 TA 模块 dotted 调用从 permissive（find_fn 拆前缀 + fresh var）变成**有类型承诺**（跨模块 typecheck 生效）
- freshness：源码 mtime vs 产物 mtime，过期重编译模块
- 与 D1 衔接：external fn 声明文件 = 纯签名产物（无字节码，实现是 dylib 懒加载）

## D4. Float 类型（历史：规划不实现；现已实现）

**结论（已过时，留档）**：Float 当时**不实现**，只修正文档；现已实现。

- 现状：Float 已落地——Val 表示就是 **NaN-boxing**（`ta.h` 的 tag 表，16-bit tag /
  48-bit payload），`ta_inline.h` 的 `val_is_float`/`val_to_double`、`OP_PUSH_FLOAT`，
  测试 `test/basic/float-arith.ta` / `float-specials.ta` 在跑，typecheck 走严格
  数值塔（`infer_num`，Int/Float 混算报错）。
- 当时的误判：以为"Int 是 48 位 tagged union、**不是 NaN boxing**、Float 暂无"
  （并据此改过 c-module.md）——现已全部过时。
- 当时的备选方案（留档）：**堆分配 double**（TAG_FLOAT → 堆指针，与
  TAG_STRING/TAG_BYTES 同模式，全精度；代价每次运算一次堆分配）；排除方案：
  TAG_FLOAT 48 位直接装 double（精度损失）、NaN boxing 重写全部 Val 表示
  （当时嫌破坏性大）——最终实际走的就是 NaN boxing。

## D6. char 字面量 = 码点 Int（语法糖）

**结论**：`'a'` 字面量 = Unicode 码点 Int（与 `97` 完全等价，零类型系统改动）。

- 现状：无 char 类型；`'` = quote 简写（lib 内 660 处，不能砍）
- gleam：有 Char 类型（UtfCodepoint）但**无字面量**，靠 `string.to_utf_codepoints` 构造
- 消歧：tokenizer lookahead——`'a'`（单引号+字符+收尾单引号）→ char 字面量；`'a`（无收尾）→ quote 简写
- 转义（`'\n'`/`'\\'`/`'\''`）复用 read_string_lit 转义解码
- 排除：独立 Char 类型 + 函数构造（重量级且不解决痛点）

## D7. GC 心智模型文档澄清（c-module.md §2/§3 已更新；其中 root 部分已被 D11 取代）

**结论**：纯文档修正，回答用户三个问题：

1. **符号表并发**（:67）：symbols 是 VM 级共享数组，`vm_intern_symbol` **无锁**（线性扫描+追加）；多 worker 线程下运行期并发 intern 存在 data race 窗口（重复 strdup/数组竞争）。实际风险小（符号集中编译期 intern）。C 模块应**初始化阶段预 intern**，运行期避免并发新建。**不加锁**（YAGNI，真出问题再说）。
2. **C 里构造的 Val 怎么被 root 保护**（:76）：root = 进程 TA 栈 + gc_roots 数组；C 局部变量不可见。构造即返回安全**原理** = 构造函数先分配后写值，无「持有旧 Val 又分配」窗口。
3. **单次分配完下次分配前不触发 GC？**（:76）：**对**。GC 同步、仅分配时堆栈碰撞触发、per-proc 独立堆互不影响、无后台 GC 线程。风险只在**跨分配持有**。

## D8. 代码风格重构（主题 8，全做）

**结论**：else if **已支持**（澄清，不改）；三处重构全做（用户：代码整洁度是需要的）：

1. **parser.ta:202**：kw 处理嵌套 if → **扁平化 else if 链**（纯样式不动逻辑；不推 match——parser 主循环风险高）
2. **tokenizer.ta:113**：is_kw 的 str.eq 链（×11）→ **提取公共函数**（one_of/查表 contains）
3. **typecheck.ta:1984**：make_builtin_env 的 extend 嵌套 let → **表格驱动 alist 遍历**（与主题 1 的 http 删除一起做，避免两次动 extend 链）

三个重构都是不改语义的编译器改动 → 走 FIXED POINT 验证 + make test 100 绿。

## D9. 测试/文档疑问（主题 9）

**结论**：
1. **无注释能否推导出错误**（type-annot-errors.ta:42）：**不能**（实验证明：无注释 `map(inc, "nope")` 通过 exit=0，带注释才报 `cannot unify String with List(Int)`）。原因：match 推断不反推参数类型，参数无标注 = fresh var。**保持现状**（标注 = 显式检查点，光谱设计；lib 大量无标注函数，反推会全爆）
2. **类型大写是命名习俗**（type-annot-hof.ta:4）：**是**——gleam/OCaml/Haskell 惯例（类型构造器大写、变量/函数小写），tinyactor 沿用
3. **len/list_ref 文档残留**（layered-type-model.md:39/60，Copilot）：已确认 typecheck 无注册（已移除），**文档两处已删** ✓

## D10. check-modules.sh 删除（主题 10）

**结论**：删除 `tools/check-modules.sh`（用户明确「我不需要这种东西啊...这个方向就不对！」）。

- Copilot 两个 bug 属实（:48 PHANTOM grep 误匹配字符串字面量；:71 warn 布尔当计数永远显示 1）但**不修**——工具删除
- 职责承接：D1 的 external fn 声明文件机制（import 强制依赖 + 签名即文档）
- 删除面：tools/check-modules.sh + Makefile:76(.PHONY)/:161-162(target) + docs/c-module.md:142-147（「make check-modules」段落）

## 实现计划（全部主题对齐后，按依赖排序进 PR #15）

1. **P1 布尔逻辑 gleam 化**（D2）：`&&`/`||` tokenizer + 砍 and/or + if 严格 Bool + logic-and-or.ta 重写 + 负测试
2. **P2 char 字面量**（D6）：tokenizer lookahead 消歧
3. **P3 代码风格重构**（D8）：parser 扁平化 + is_kw 公共函数 + typecheck 表格化
4. **P4 external fn 声明文件**（D1）：typecheck 加载声明文件 + demo/http 迁移 + 删 http extend 行 + 删 check-modules
5. **P5 模块级编译 B1'**（D3）：模块产物 + 签名表 + import 链接（在 P4 声明文件机制上做）

每个编译器改动 commit：FIXED POINT VERIFIED + make test 100 绿。

已在 D2 覆盖：if 条件 typecheck 要求 Bool，非 Bool 编译期报错；运行时 OP_JUMP_IF_FALSE 的 falsy={nil,false} 语义保留（防御性）。

## 待对齐主题（review comments 剩余）

- 主题 3：Float 类型（NaN pointer 技巧、未来规划）
- 主题 4：GC 心智模型文档澄清（符号并发、root 原理）
- 主题 7：char 类型支持（tokenizer 数字比较）
- 主题 8：代码风格（else if、公共函数提取、表格驱动注册）
- 主题 9：测试/文档疑问（无注释推导、类型命名习俗、len/list_ref 文档残留）
- 主题 10：Copilot 代码问题（check-modules.sh PHANTOM grep 误匹配、warn 计数 bug）

## D11. GC 分配时就地收集 + per-proc 门 + arena 收集点增长（issue #136；#119 phase2）

**结论**：GC 的触发点从"每条指令边界的 `gc_pending` 检查"（`vm_run_proc` 循环顶）
改为**分配时**：`proc_heap_alloc` 在堆超过 `gc_trigger`（或 `TA_GC_STRESS` 要求）时
**就地**收集，但仅在 `Proc.gc_gate == 0`（"当前没有不可根化的活引用"）时。VM 主循环
内不再有任何 GC 检查。arena **经收集点增长**：起始 512 B 空转缓冲，首次堆分配切到
`TA_PROC_HEAP0`（512 B），之后逐次翻倍直到 `TA_ACTOR_HEAP` 上限（默认 64 MiB），由此
继续**不需要 rooting**：`gc_root_push`/`gc_root_pop`/`GC_ROOTS_SCOPE`/`Proc.gc_roots*`、
`proc_grow`、`gc_fixup_heap_pointers`（及其 `fixup_buffer`/`fixup_val_in_range`）依旧
不存在。

**安全点定义**（替代旧的"唯一 opcode 边界"）：

1. **门开时的分配点**：`proc_heap_alloc` 里 `gc_pending` 或堆超 trigger 且
   `gc_gate == 0` ⇒ 就地 `gc_collect`。GC 的 root 集**只有 TA 栈**，所以调用这个
   分配点的瞬间，该 proc 的所有活引用都必须在栈上（或在 gate 罩住、不会被收集的
   区域里）。
2. **门关时的延迟 + 补收**：无法满足上面前提的区段——把多个活堆引用放在 C 局部 /
   malloc worklist 里横跨分配（C 模块回调、`val_deep_copy` 的迭代 worklist、嵌套
   `val_pair` 链、将死的 proc）——用 `proc_gc_enter`/`proc_gc_leave` 把 `gc_gate`
   加/减一。门关时分配只置 `gc_pending`；门重新开为 0 且区段的活值已根化后，由
   `proc_gc_drain` 补收（如 `OP_CCALL_NAME`/`OP_RECV*` 在把结果压栈之后）。
3. **延迟收**：区段出来时若活值还没根化（`val_deep_copy` 的返回值、将死 proc 的堆），
   就不补收，让请求挂着——下一个门开的分配点的那次 `proc_gc_drain` 自然会消费它。

**能重写为栈纪律就不套门**：`OP_CONS` 过去先 `pop` 到 C 局部再 `val_pair`，现在改为
"先分配 → 再从栈槽 `proc_peek` 读 car/cdr → `sp += 2` → 压回新 pair"（GC 只转发栈
槽，按值读即安全）。`OP_CLOSURE`/`OP_PUSH_STRING` 本就"先分配、后从栈或字节码读"，
无需门；`OP_PUSH_FLOAT` 只是从字节码取操作数，不分配，更无需门。

**一条保证**（仍成立）：**arena 不移动**。`Val` 是**绝对指针**，但 buffer 只允许在
`heap_ptr == 0`（无对象 ⇒ 无指针指向它）时扩容/搬迁，之后终生冻结。收集只是把存活
对象在两个半区间复制、并**转发 TA 栈**；因此指向堆内缓冲区的裸指针
（`char *` into `HeapString->data`）只要在安全点不在 C 局部里横跨分配，就仍然有效。
C 模块的"不需要 root"承诺不变：`OP_CCALL_NAME` 用门罩住整个回调，回调期间不发生
收集，回调返回后结果先压栈再补收。

**代价 / 取舍**（明确记录）：

- **栈/堆相撞不再靠 GC 救**：`proc_push` 撞上堆时只能扩栈（堆为空）或 **fatal**
  （堆非空）。这是刻意的——arena 已是全部可用空间，GC 也换不出更多。
- **arena 耗尽是 fatal**（`ta_arena_fatal`：打印占用明细 + `abort()`，rc 134），
  不再返回 `nil` 让程序带着半个结构继续跑。与 #129 的 procs[] 越界同一原则。
- **懒增长**：64 MiB × 1M idle actor 不可行，所以空转 actor 只占 512 B 空转缓冲，
  **首次堆分配**才切到 512 B 的 `TA_PROC_HEAP0`，此后只在收集点翻倍增长（收集必须
  搬家，只发生在 GC 门内）；不分配的 actor 永不增长。
- **存活集阈值**：`gc_trigger = 2 × 上次存活集`（下限 4 KiB，**无上限**——gc.c 注释
  明言「No ceiling」），收集后堆=存活集，余下留给栈与后续增长。默认 64 MiB 下实测
  `test/actor/million-actors.ta` 单 actor 峰值 32 KiB。
- 顺带修掉两个隐患：`gc_copy_obj` 增加**先检查后 memcpy** 的边界 fatal（旧代码在
  半区放不下时静默越界写坏堆）；删除每轮收集对整块 tospace 的 `memset`（读方只扫
  到 `gc_to_size`，分配槽位由 `proc_heap_alloc` 清零）——否则每轮收集都 touch 整个
  arena。

**影响面**：ta.h / ta_inline.h / gc.c / vm.c / val.c / scheduler.c / api.c /
lib/demo.c + `docs/c-module.md` §3 重写 + 新增
`test/crash/arena-exhausted.ta`（fatal 路径回归）；phase2 追加
`test/run_gc_tests.sh` 的 `TA_GC_STRESS=1` 轮（门纪律执行器）。

## D12. actor primitive 从专用 opcode 迁到 OP_BUILTIN 表（PR E / phase 4a+4b）

**结论**：`spawn / spawn_main / spawn_clos / send / recv / recv_peek / recv_commit /
recv_after / monitor` 这 **9 个 actor primitive 不再是独立 opcode**，改为
`OP_BUILTIN`（opcode 49）+ **1 字节静态索引**，由 `builtin_table[]`（`src/builtin.c`）
分派。指令发射编码：

```
OP_BUILTIN idx [原操作数…]        // spawn/spawn_main 保留 4 字节 fn_id
```

**动机**：

- **这些原语都在冷路径上**：一次 actor 调用（spawn / send / recv / monitor）本来就
  远贵于一次间接调用——它们要碰 scheduler、mailbox、可能阻塞整个 proc。为它们各留
  一个 opcode，收益只体现在"省掉一层间接调用"，而那层调用的成本相对原语本身可以
  忽略。
- **opcode 是"最贵的一种扩展点"**：每加一个 opcode 要同步维护多处镜像——
  现行是 `lib/bootstrap/compile.ta` 的 `const OP_*`（编码权威）↔ `ta.h` 的 `LOP_*`
  枚举、`compile.ta` 的 `B_*` ↔ `src/vm.c` 的 `LB_*`（`tools/check_opcode_mirrors.py`
  把守）——更早的镜像（`codegen.ta` 镜像常量、`vm.c dispatch_table[]`、
  `api.c instr_len[]`）已随 `codegen.ta` 删除与 VM 重构一并消失。
  而 primitive 的集合还在长（`monitor`、
  `recv_after` 都是后加的），每加一个就动一次这套镜像，冲突面大且容易漏。
- **让热路径保持"全是便宜指令"**：dispatch 表里现在只剩 cons/car/cdr/类型判断/
  算术/跳转/调用，每条都确实比一次间接调用便宜。

**代价 / 取舍**（明确记录）：

- **多一层间接调用**：每条原语指令多一次 `builtin_table[idx]` 查表 + 间接调用，
  以及 `OP_BUILTIN` handler 里一次 SP 回写/重载。冷路径上不可测。
- **`idx` 是手写契约**：`BuiltinId`（ta.h）/ `B_*` 常量（`compile.ta`）/
  `LB_*`（`src/vm.c`）/ `builtin_table[]`（builtin.c）必须对齐。其中
  `compile.ta` ↔ `ta.h`/`vm.c` 的**数值**由 `check_opcode_mirrors.py` 逐项对照
  （`OP_*`↔`LOP_*`、`B_*`↔`LB_*`：常量名、数值、计数，枚举留洞也报错）；`builtin_table[]` 用指定初始化器（`[BUILTIN_SEND] = b_send`）
  按名字索引，写错名字编译器直接报错，但**漏一项会留下 NULL 槽位**，这部分仍靠
  review 兜住（脚本不解析该表）。旧的"每个原语一个 opcode"模式下这个契约是隐式
  的（opcode 号本身就是身份）。
- **编号留洞**：删掉的 9 个编号**不回收**，在 `ta.h` 的 `OpCode` 枚举里以
  `OP_RESERVED_<PRIMITIVE>` 保留（34-40 / 42 / 48；`OP_SELF = 41` 仍然是真的
  opcode——它便宜且极热）。好处是幸存 opcode 编号一个都不动，`bootstrap.tabc`
  之外的任何既有二进制/注释/文档引用不会静默指错；代价是枚举里留了 9 个洞，
  命中它们等于未定义 opcode，与越界 opcode 走同一条 unknown-opcode 报错路径。
- **`.tabc` 版本 v2 → v3**（当时的机制）：v2 镜像里原语还是独立 opcode，v3 VM
  无法解释它，当时的 loader **直接拒绝**非 v3（错误信息写明版本号）。**现行
  `src/main.c` 已无版本检查**（grep 无 v3/version/magic/format 报错文本），版本
  检查随旧 loader 一并删除。这也是 v1/v2 兼容代码路径彻底失效的分界点。

**新 opcode 的准入门槛（本 PR 之后的规则）**：加 opcode 必须满足
**(a) 在每条指令都跑的热路径上，且 (b) 明确比一次间接调用更便宜**（带内联的
一两个字节操作、单次数组索引/算术、无函数调用的栈操作）。不满足就加进
`builtin_table[]`——那里只花一次间接调用，且不占需要同步维护的镜像位置
（`compile.ta`/`ta.h`/`src/vm.c` 的常量与枚举，`check_opcode_mirrors.py` 把守）。
反例即本次迁走的 9 个原语（冷、且重）；
正例是留在表里的 cons / car / cdr / 类型判断 / 算术 / `OP_SELF`。

**影响面**（当次 PR 的改动清单，历史记录）：`ta.h`（枚举改名 + 保留 9 个洞）/
`src/vm.c`（删 9 个原语的独立 handler，保留 `OP_SELF`/`OP_BUILTIN` 与其 path）/
`src/api.c`（`rebase_code` 删 `OP_SPAWN*` 分支）/ `docs/ta-language-spec.md` 不变
（语法没动，只动了字节码编码）。`.tabc` 版本号与版本检查（原 `ta.h`/`api.c`）、
`lib/bootstrap/codegen.ta` 及其 `emit_builtin`/`serialize_tabc` 均已随后续重构删除，
不在现行代码中。

## D13. 内建类型名统一大写（类型 vs 类型变量的唯一定则）

**结论**：所有内建类型名统一为**首字母大写**——`Int`、`Float`、`String`、`Bool`、
`Symbol`、`Pid`、`List`、`Pair`（内部 base 类型 `Nil`、`Never` 同样上移）。类型位置的
命名规则收敛为**唯一一条、无例外**：

> 类型位置上的**首字母大写**标识符 = 具体类型（内建类型或已声明 ADT 名）；
> **首字母小写**标识符 = 类型变量（隐式多态）。

不做兼容层、不保留旧拼写（语言尚未 1.0 GA，不为历史做法背包袱）。

**理由**：认知负担要最低，规则就不能有例外。此前只有 `Pid` 是特判——parser 同时接受
`'pid` 与 `'Pid`（typecheck 里 4 处 `if f == 'pid` / `if f == 'Pid'` 分支），而
`int`/`string`/`bool`/`float`/`symbol`/`pair` 一律小写。用户读到 `Pid` 以为"大写即类型"，
再用 `Int` 就报错——规则有洞，认知成本高。改后 `Xxx` 恒为具体类型、`xxx` 恒为类型变量，
与 ADT（`type Color { Red }`）/ 类型参数（`type MyList(a) {...}`）的既有大写约定天然一致。

**实现**（分层，落在编译器层，不碰 VM）：

- `lib/bootstrap/typecheck.ta`：base 类型符号改名（`'int`→`'Int` 等），并把原先
  分散的 `'pid`/`'Pid`、`'int`/`'string`… 特判**折叠**为统一的"大写名 = 具体类型、
  小写名 = 类型变量（查 param_map）"判定（`is_uppercase_sym` / `t_base`），特判分支净减。
- `lib/bootstrap/modsig.ta`：签名格式版本 `TASIG1`→`TASIG2`。类型表示变了，必须让
  旧 `.build/modules/*.sig` 缓存失效（否则叶子模块会复用旧编译器写下的签名，出现
  "cannot unify Bool with bool" 这类鬼影）。
- `lib/bootstrap.tabc`：`make bootstrap` 重生成（fixed point 校验通过）。
- 下游：`lib/*.ta` 注解、`test/compiler/parser-ast.ta` 期望（带标签字段类型经
  `parse_expr` 编成 `(quote Int)`，与用户 ADT 字段同形）、`tools/kernfuzz/gen.py`
  产出、`test/kernfuzz-frozen/` 快照与 tc-negative、`docs/ta-language-spec.md`。

**取舍**：用标识符大小写承载"具体类型 vs 类型变量"的语义（同 Rust/OCaml 的
`Vec<T>` vs `'a`），代价是"改类型名要动源码全量注解"——一次性成本，换永久的一致性。
