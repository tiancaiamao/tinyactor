# UX log — 使用体验记录

写这门语言的代码时踩到的「不舒适」，逐条追加。**先记，不修**：由维护者周期性
review，决定转 issue / 改设计 / 忽略。规则见 AGENTS.md「用户体验记录」。

条目格式：

```
## YYYY-MM-DD · 一句话标题
- 场景：当时在写什么
- 不适：多花了什么代价（多写、写错、查半天、绕过类型系统…）
- 想要的：如果语言/工具顺手，本该是什么样
- 去向：（留空 = 待 review；采纳后写 → 已解决：<PR/issue>）
```

---

## 2026-11-09 · 多种形状放同一位置，没有类型可写

- 场景：给 `lib/bootstrap/ast.ta` 的 `const_atom` 补类型注解。它把常量摊成裸原子
  —— nil / bool / int / symbol / string / `('float text)` 六种并存。
- 不适：HM 里没有 union，也没有 any。以前靠「match binder 不承重」这个洞蒙混过去：
  写得出，但类型是假的。要把 binder 接通字段类型（#442），这处立刻没有合法写法。
- 想要的：一个「这里本来就是动态」的显式写法。现在有 `Atom` 了（同批落地），
  但**每加一个逃逸口都要掂量它被滥用的成本**——`Atom` 关掉的是下游全部检查。
- 去向：`Atom`（docs/ta-language-spec.md「动态逃逸」），同 PR 落地。

## 2026-11-09 · 返回注解在 match-bodied 函数上不承重

- 场景：读 typecheck 的自测期望，发现「注解 Int 实际返回 String」能过。
- 不适：注解是给人看的契约，静默不生效比没有注解更危险——你会相信它。
- 想要的：注解即约束，违反就报。已随 #442 落地（binder 取构造子字段类型）。
- 去向：→ 已解决：#442

## 2026-11-09 · 注解里的类型名拼错，报错指向调用点而不是声明

```ta
fn f(s : Strng) -> Strng { s }       // 拼错 String
fn main() { print(f("a")) }
```

```
[E0001] in function 'main' (line 2): arg 1 of f: cannot unify String with Strng
```

- 不适：`Strng` 被当成一个「不透明类型」收下，没有任何「未知类型名」诊断；错误
  冒在调用点，而 typo 在函数签名上。人会先去怀疑 `f` 的实参，而不是自己拼错了。
- 想要的：声明处直接 `unknown type 'Strng'`（与 #443 的 `unknown constructor`
  同一个家族：大写开头的名字总要能对上一条声明）。
- 去向：（待 review）

## 2026-11-09 · 没跑过 CI 的后端，等于没跑过

- 场景：KL→C 后端本地全绿，接进 CI 后第一轮就炸 `#error requires clang
  musttail`，第二轮又炸 `lib/time.so: undefined symbol: tls_current_proc`。
- 不适：两处都是「macOS 恰好能过」的环境差异（`cc` 不是 clang；Linux 要
  `-Wl,--export-dynamic`），本地跑一万次也看不见。零门禁的代码不能算绿。
- 想要的：新管线第一天就挂进 CI 的既有 job（而不是等「以后加个 job」）。
- 去向：→ 已解决：#446（backend-gate 加 `make test-kl-c`）

## 2026-11-09 · `make test` 全绿不等于 CI 全绿

- 场景：给 `test/basic/` 加了一个用例，本地 `make test` 326 PASS，CI 却红在
  `kernfuzz-snapshot-check`——新语料缺冻结快照。
- 不适：本地没有「CI 等价」入口。`make test` 只串了 11 个 category，CI 另外还跑
  `backend-gate` / `kernfuzz-snapshot-check` / `test-kl-c` / `fmt-check` /
  bootstrap fixed point，得靠记性逐个补跑；忘一个就是一轮 15 分钟的黄灯。
- 想要的：一个 `make pre-pr`（或让 `make test` 直接包含这些 gate），跑完就等于
  跑完 CI。
- 去向：（待 review）
