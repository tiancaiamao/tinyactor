# 设计决策记录（PR #15 review 对齐）

> 与用户在 PR #15 review comments 对齐过程中确认的设计决策。
> 每个决策：结论 + 理由 + 影响面。实现前以此为准。

## D1. 外部 C 模块类型声明机制（external fn）

**结论**：独立声明文件 `lib/<mod>.ta`（与 dylib 同名），语法 `external fn` + 可选绑定。

```ta
// lib/mymod.ta —— mymod.dylib 的声明文件（外部模块作者只写这个 + .c）
external fn double(x: int) -> int = "mymod" "double"   // 显式绑定（C 侧名不同时）
external fn greet(name: string) -> string              // 缺省 = 模块名.函数名同名
```

- **必须 `import mymod` 才生效**：不 import 调用 `mymod.foo` 报 undefined function（强制显式依赖，依赖关系清晰）
- 类型语法**无需扩展**：现有标注语法已覆盖 int/string/bool/nil/pid/pair/泛型（'a）/箭头（a -> b）/复合（List(int)）；`pair` 经 `t_base` 自动得到 t_pair()
- **内置静态模块（str/net/file/buf/vm）留在 typecheck extend 链**：它们是 tavm.c 静态注册，非 dylib，声明文件机制不适用（用户：标准库在 typecheck 写问题不大）
- **迁移验证案例（一步到位）**：`demo`（建 lib/demo.ta + 测试加 import demo，成为完整模板）+ `http`（建 lib/http.ta + 删 typecheck.ta e60-e62 的 http 行；kv_server/http_server 已 import，回归验证）
- check-modules.sh 的职责被声明文件机制替代 → 用户明确「不需要这种东西」（去留见 D6）

## D2. 布尔逻辑对齐 Gleam

**结论**：外部语法 `&&`/`||` 中缀，AST 保持 `('and a b)` / `('or a b)`。

- TA 源码写 `a && b` / `a || b`；`&&` 优先级高于 `||`（gleam 一致：`a && b || c` = `(a && b) || c`）
- parser 转换：`&&`/`||` → `('and x y)` / `('or x y)`；**codegen 的 compile_and/compile_or 与短路逻辑不动**（7de77f4 保留）
- **砍掉外部 `and`/`or`**（中缀 + `(and ...)` 前缀形式），恢复为普通标识符
- **语义严格 Bool**：操作数 + if 条件 typecheck 必须 unify 到 Bool，非 bool 编译期报错；and/or 返回 Bool（不再是值返回，0 不是 truthy）
- 配套：logic-and-or.ta 重写为 bool 语义；新增 if 严格检查 + `&&`/`||` 负测试；tokenizer 支持 `&&`/`||` token
- 影响面已审计：lib/*.ta 内部零 and/or 使用、if 条件全部是 bool 表达式 → 收紧零破坏

## D3. 模块级编译（B 方案：编译期链接）

**结论**：模块预编译为 `.tabc` 段 + `.sig` 签名表，主程序 import 时**编译期拼接（链接）**进输出，仍是单文件 .tabc。

- 产物放 `lib/<mod>.tabc`（与源码同目录，仿 dylib 约定）；签名表 `lib/<mod>.sig`
- **tavm/运行命令零改动**：VM 多模块拼接（vm_append_module：符号去重 + rebase + 重定位）已实现；输出仍单文件
- **非 pub 不导出**：签名表只含 pub 函数（pub 是真正的接口边界）；非 pub 编译期不可见（主程序 typecheck 拦截）
- **命名空间隔离推迟**：符号冲突现阶段未爆发，不做模块前缀系统（光谱原则，不过度设计）
- **附带收益**：`math.abs` 等 TA 模块 dotted 调用从 permissive（find_fn 拆前缀 + fresh var）变成**有类型承诺**（跨模块 typecheck 生效）
- freshness：源码 mtime vs 产物 mtime，过期重编译模块
- 与 D1 衔接：external fn 声明文件 = 纯签名产物（无字节码，实现是 dylib 懒加载）

## D4. float 类型（规划，不实现）

**结论**：float 进路线图但**当前不实现**，只修正文档。

- 文档 c-module.md 已修正：int 是 **48 位有符号**（高 16 位 tag 的 tagged union，**不是 NaN boxing**，可用位数不是 64）；float 暂无
- 规划形态：**堆分配 double**（TAG_FLOAT → 堆指针，与 TAG_STRING/TAG_BYTES 同模式，全精度；代价每次运算一次堆分配）
- 排除方案：TAG_FLOAT 48 位直接装 double（精度损失）、NaN boxing 重写全部 Val 表示（破坏性大）

## D6. char 字面量 = 码点 int（语法糖）

**结论**：`'a'` 字面量 = Unicode 码点 int（与 `97` 完全等价，零类型系统改动）。

- 现状：无 char 类型；`'` = quote 简写（lib 内 660 处，不能砍）
- gleam：有 Char 类型（UtfCodepoint）但**无字面量**，靠 `string.to_utf_codepoints` 构造
- 消歧：tokenizer lookahead——`'a'`（单引号+字符+收尾单引号）→ char 字面量；`'a`（无收尾）→ quote 简写
- 转义（`'\n'`/`'\\'`/`'\''`）复用 read_string_lit 转义解码
- 排除：独立 Char 类型 + 函数构造（重量级且不解决痛点）

## D7. GC 心智模型文档澄清（c-module.md §2/§3 已更新）

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
1. **无注释能否推导出错误**（type-annot-errors.ta:42）：**不能**（实验证明：无注释 `map(inc, "nope")` 通过 exit=0，带注释才报 `cannot unify string with List(int)`）。原因：match 推断不反推参数类型，参数无标注 = fresh var。**保持现状**（标注 = 显式检查点，光谱设计；lib 大量无标注函数，反推会全爆）
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

已在 D2 覆盖：if 条件 typecheck 要求 Bool，非 bool 编译期报错；运行时 OP_JUMP_IF_FALSE 的 falsy={nil,false} 语义保留（防御性）。

## 待对齐主题（review comments 剩余）

- 主题 3：float 类型（NaN pointer 技巧、未来规划）
- 主题 4：GC 心智模型文档澄清（符号并发、root 原理）
- 主题 7：char 类型支持（tokenizer 数字比较）
- 主题 8：代码风格（else if、公共函数提取、表格驱动注册）
- 主题 9：测试/文档疑问（无注释推导、类型命名习俗、len/list_ref 文档残留）
- 主题 10：Copilot 代码问题（check-modules.sh PHANTOM grep 误匹配、warn 计数 bug）