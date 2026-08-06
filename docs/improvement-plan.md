# TinyActor 改进计划 (Improvement Plan)

> 北极星：**类型安全的 Erlang**（语法类 Gleam）。类型安全尽可能，但不极致
> （不追求 typeclass/traits；HM + ADT 泛型为止，不为类型安全付出过高实现/使用代价）。
> 自举已完成，进入"用起来"阶段。

## 执行顺序（用户确认）

```
阶段 A（bootstrap 工程债）→ 阶段 B（错误信息）→ 阶段 C（HOF 类型标注）
→ 阶段 D（真实程序）→ 阶段 E（C 交互）
→ 之后 D/E 反哺 A/B/C，形成持续改进环
```

LSP 明确**不着急**（老派用户，先做基础）。

---

## 阶段 A — Bootstrap 工程债（必须先还，浮沙筑高台）

- [x] A1. Makefile 产物新鲜度检查：`bootstrap.tabc` mtime < 任一 lib/*.ta 源码 → fail
      （当前靠人眼 `stat` 对比，已两次踩坑）
- [x] A2. CI gate：`make bootstrap-selfhost` 固定点校验 + 新鲜度检查进 PR 流程
- [x] A3. 消除管道掩盖失败：`make bootstrap 2>&1 | tail -1` 的退出码陷阱
      （Makefile 内部检查编译真实结果，不接受管道退出码：产物 test -s
      验证 + 判定行最后输出 + caller pipefail 传播真实状态）

## 阶段 B — 错误信息强化（基础功能，查问题成本是几倍开销）

- [x] B1. 类型错误带上下文：出错表达式所在行 + 片段 + 具体是哪个参数/分支
      （嵌套 ctx 链根符号提取函数名；find_call_site 定位失败调用行 +
      源码片段；无 detail 回退函数定义行）
- [x] B2. unify 失败分层信息：哪个函数调用、第几个参数、期望 vs 实际类型
- [x] B3. 编译错误分类 + 错误码（结构化输出，为 LSP 铺路）

## 阶段 C — 分层类型模型（map 不能 typecheck 的根因修复）

> 设计原则（用户确认）：类型检查只发生在上层语言；下层实现是无类型的。
> 但底层原语**不是完全不承诺类型**——按语义通用性取光谱：
> - **真·通用原语**（cons/car/cdr/print/类型谓词）：保持宽松多态（forall）
> - **语义绑定原语**（str.to_int / file.* / net.*）：必须带具体类型
> - 例：`str.to_int('Red)` 必须被拒（symbol ≠ string）——现状已正确
>
> 上层安全面：ADT 构造器 + 模式匹配 + 带签名 fn + 类型化封装函数
> （"kons"只是举例，是否引入按需求决定，不是必须项）

- [x] C1. parser 支持函数类型标注 `a -> b`：
      `parse_type_after_colon`（parser.ta:354）只解析单 token，不处理 `->`
      → 加 arrow 解析（右结合，支持 `a -> b -> c`），产出 `('arrow A B)`
      typechecker 已有完整 arrow 支持（t_arrow/unify/subst），预计只改 parser
- [x] C2. 验证 HOF 全通：map / foldl / filter / compose 全部 typecheck + 运行时正确
- [x] C3. **逐原语类型承诺审计**（光谱落地）：
      - 已正确：str.to_int/str.concat/str.length 等带类型；cons/car/cdr 宽松
      - 待收紧：`str.to_sym: forall(a, string -> a)` 应承诺 `string -> symbol`；
        `str.sym_to_str: forall(a, a -> string)` 应承诺 `symbol -> string`
        （`str.sym_to_str(42)` 现在能通过编译，是过度放宽）
      - 产出：builtin 类型承诺清单（每个原语标 宽松/绑定）
- [x] C4. 分层类型模型文档化（docs/）：上层 typed surface ↔ 下层 untyped，
      光谱规则、kons 类封装的定位

## 阶段 D — 真实程序（验证 + 暴露短板）

- [x] D1. TA 写真实 HTTP 服务：KV store + 限流 + supervisor 雏形
      `example/scripts/kv_server.ta`：main→spawn supervisor（monitor KV，DOWN 重启）
      + spawn accept_loop（令牌桶限流 10 burst / 5 refill-per-sec）→ 每连接 spawn handler。
      路由：GET/PUT/DELETE `/kv/<key>`、`/health`、`/stats`、`/crash`、404/405/429。
      冒烟全绿：存取删/404/405/429/crash→DOWN→sup 重启空 store。
- [x] D2. 暴露的短板反哺 A/B/C（错误信息、标准库、调试体验、性能）
      已反哺：#3 str.eq bool 语义统一、#4 len/list_ref 幻影消除、#1 str.chr、
      #1 字面量转义（B 阶段完成，见下）。#2/#6/#7 跳过（大工程/可接受）。

### D1 暴露的短板清单（D2 候选）

1. **[x] 字符串字面量转义**：`"\r\n"` 字面 4 字符 → 已加 tokenizer 级转义解码：
   `\n \r \t \\ \" \'` + `\xNN` 十六进制字节转义；未知转义按字面保留。
   动 tokenizer（编译器核心），FIXED POINT VERIFIED + 全量回归绿。
   `str.chr(13) + str.chr(10)` 仍可用作运行时构造 CRLF 的替代。
2. **dotted module 调用无类型承诺**：`str.eq`/`http.parse_request`/`net.*` 全是
   permissive fresh var → 类型错误全推迟到运行时。C 阶段光谱规则应覆盖模块函数。
3. **[x] `str.eq` 返回 int 0/1 而非 bool**：if 判定"非 nil 即真"（int 0 也 truthy），
   裸 `str.eq(a,b)` 恒真 → 全部请求路由到 /health 分支（表现为全返回 "ok"）。
   代码库惯例 `str.eq(a,b) == 1`，但 typecheck 不拦裸用。B/C 候选：bool 类型收紧 or
   dotted 函数签名。
   → 已修（D2 #3）：str.c 的 str_eq_fn 改返回 val_true()/val_false()（与 OP_STR_EQ 一致），
   lib 33 处 + kv_server 10 处 `str.eq(X) == 1` → 裸用 `str.eq(X)`。运行时兑现 typecheck 的
   bool 承诺。fixed point 验证 + make test 92 绿 + kv 冒烟全对。
4. **[x] `len`/`list_ref` 是幻影 builtin**：typecheck 有、runtime 无 → kv_server 被迫手写
   list_length。
   → 已修（D2 #4）：从 typecheck 移除 'len/'list_ref 幻影注册（上层不承诺 runtime 没有的
   东西——与 #3 同哲学）。lib/parser.ta 本就自带 `fn list_ref` TA 实现，自给自足；用户
   自定义 `fn len`/`fn list_ref` 即可用（typecheck 给出清晰 undefined function 错误而非
   运行时崩溃）。注：曾尝试加 OP_LEN/OP_LIST_REF opcode，实证破坏 bootstrap fixed point
   （codegen dispatch 引入 'len/'list_ref 会把 lib 同名 TA 函数调用重写成新 opcode →
   mismatch + 死循环），弃用。
5. **`and`/`or` 是死语法**：codegen 有 compile_and/or，parser/tokenizer 无 → 写 `a and b`
   解析错乱级联假类型错误。A/B 候选：实现或显式报错。
6. **import typecheck 栈溢出**：脚本 import 大模块（typecheck.ta）重新编译 → 栈溢出段错误；
   bootstrap.tabc（预编译）路径正常。驱动构建是 ground truth。
7. **限流快速 close 偶发 RST**：token 耗尽时 accept→立即 close → curl 偶发 000 而非 429
   （客户端侧 RST）。可接受，记录之。

## 阶段 E — C 交互（用户强调：至关重要，要做对做易用）

> 目标：**普通人**能自己写 C 模块（不只是调用 C），
> GC 交互的心智负担 + 安全问题做到 Lua 级别（Lua 是少数做对的语言）。

- [x] E1. 设计 C 模块扩展机制：模块注册、导出、类型映射、错误约定
      （`docs/c-module.md`；核心缺口已补：codegen 检测 dylib 生成懒加载调用，
      普通 C 模块零配置可用）
- [x] E2. GC 安全：root 注册 / 引用管理 / 生命周期 / 析构 /
      明确文档化的心智模型（哪些内存 GC 管、哪些模块管、边界在哪）
      （`docs/c-module.md` §3：三条规则 + 边界表；机制在 ta.h
      公开 tls_current_proc + GC_ROOTS_SCOPE）
- [x] E3. 示例 C 模块模板 + 回归测试（普通人照着模板能写）
      （`lib/demo.c` 模板 + `test/module/demo-c-module.ta`，93/93 测试绿）

---

## 反哺环（持续）

阶段 D/E 的每一次使用发现 → 回到 A/B/C 改进 → 再使用 → 形成闭环。