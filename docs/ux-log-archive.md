# UX log — 归档

`docs/ux-log.md` 只留**未解决**的条目。一条记录一旦解决（或有了明确结论「不做」），
整条**原文搬到这里**，末尾补一行 `→ 已解决：<PR/issue>` / `→ 结论：…`。

搬走而不是删掉，是因为「当初为什么这么别扭」本身就是设计依据；留在活跃日志里
则会让真正待办的东西被淹没。活跃日志只该是**待办清单**。

---

## 已解决

### 2026-11-09 · 多种形状放同一位置，没有类型可写

- 场景：给 `lib/bootstrap/ast.ta` 的 `const_atom` 补类型注解。它把常量摊成裸原子
  —— nil / bool / int / symbol / string / `('float text)` 六种并存。
- 不适：HM 里没有 union，也没有 any。以前靠「match binder 不承重」这个洞蒙混过去：
  写得出，但类型是假的。要把 binder 接通字段类型（#442），这处立刻没有合法写法。
- 想要的：一个「这里本来就是动态」的显式写法。现在有 `Atom` 了（同批落地），
  但**每加一个逃逸口都要掂量它被滥用的成本**——`Atom` 关掉的是下游全部检查。
- 去向：`Atom`（docs/ta-language-spec.md「动态逃逸」），同 PR 落地。
  → 已解决：#447（`Atom` 随「binder 接通构造子字段类型」同批落地；名字本身的
  问题另记 `docs/ux-log.md`「`Atom` 这个命名」）

### 2026-11-09 · 返回注解在 match-bodied 函数上不承重

- 场景：读 typecheck 的自测期望，发现「注解 Int 实际返回 String」能过。
- 不适：注解是给人看的契约，静默不生效比没有注解更危险——你会相信它。
- 想要的：注解即约束，违反就报。已随 #442 落地（binder 取构造子字段类型）。
- 去向：→ 已解决：#442

### 2026-11-09 · 自己定义 `fn f(a, b)`，import 的库就报假错

- 场景：写测试文件时给辅助函数起名 `f`，同时 `import "tinyactor:lib/list"`。
- 不适：编译失败，错误全在**库内部**：
  `[E0003] in function 'list.map': arg 1 of cons: wrong number of arguments for 'f' (got 1, want 2)`。
  原因是 arity 表（#376）按**裸名**登记全程序函数元数，库内 `map(f, lst)` 的形参
  也叫 `f`，于是库里的 `f(car(lst))` 被按我的 `f/2` 检查。`option.ta`、`dict`、任何
  形参名为 `f`/`g`/`cmp`/`key`/`k` 的库都中招 —— 换句话说：用户不能给函数起名 `f`。
  假错还指向库，第一反应是「库写错了」。
- 想要的：arity 检查只作用于**未被子作用域遮蔽**的全局名（`f` 是 `map` 的形参，
  就该退出全局 arity 表）；或者干脆按符号而不是按名字。
- 去向：→ 已解决：#449（typecheck 的 env 改成 `[局部绑定...] [arity 表] [全局 define...]`，
  `locally_bound` 只查表上方的绑定；局部名遮蔽同名 define 与同名 builtin，不再查 arity）

---

## 工程流程（不属语言使用体验，移出活跃日志）

下面两条是**维护者自己**的流程摩擦，不是「用这门语言写代码」的体验；活跃日志不收
这类记录（AGENTS.md「用户体验记录」已写明范围）。

### 2026-11-09 · 没跑过 CI 的后端，等于没跑过

- 场景：KL→C 后端本地全绿，接进 CI 后第一轮就炸 `#error requires clang
  musttail`，第二轮又炸 `lib/time.so: undefined symbol: tls_current_proc`。
- 不适：两处都是「macOS 恰好能过」的环境差异（`cc` 不是 clang；Linux 要
  `-Wl,--export-dynamic`），本地跑一万次也看不见。零门禁的代码不能算绿。
- 想要的：新管线第一天就挂进 CI 的既有 job（而不是等「以后加个 job」）。
- 去向：→ 已解决：#446（backend-gate 加 `make test-kl-c`）

### 2026-11-09 · `make test` 全绿不等于 CI 全绿

- 场景：给 `test/basic/` 加了一个用例，本地 `make test` 326 PASS，CI 却红在
  `kernfuzz-snapshot-check`——新语料缺冻结快照。
- 不适：本地没有「CI 等价」入口。`make test` 只串了 11 个 category，CI 另外还跑
  `backend-gate` / `kernfuzz-snapshot-check` / `test-kl-c` / `fmt-check` /
  bootstrap fixed point，得靠记性逐个补跑；忘一个就是一轮 15 分钟的黄灯。
- 想要的：一个 `make pre-pr`（或让 `make test` 直接包含这些 gate），跑完就等于
  跑完 CI。
- 去向：→ 结论：**不做**。没有任何地方承诺「`make test` 过了 CI 就过」，本地也
  不可能复刻 Linux 侧；`make pre-pr` 本地要跑十几分钟，不如让 CI 说真话。真正的
  需求是「忘了补哪个 gate」这类信息由 CI 直接报出来，而不是本地养一个影子 CI。
  （已知硬约束：新语料必须同步冻结快照，见 AGENTS.md「Build & Test」。）