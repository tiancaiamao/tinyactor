# Bootstrap Convergence Plan

## 现状

TinyActor 项目处于自举收敛过程中。当前 C tinyactor 作为种子编译器，读取 `lib/bootstrap.tabc`（TA 编译器的字节码），用 TA 编译器编译 `.ta` 源文件。

## 已完成

### Phase 0 - TA 解析器就绪 ✅
- lib/parser.ta, lib/tokenizer.ta, lib/codegen.ta, lib/typecheck.ta 已实现

### Phase 1 - reader_ta.c GC 修复 ✅ (commit 7a6c878)
- Fixpoint 确认，所有 195 个测试通过

## 当前任务：Phase 2 - 模块加载从 C reader 切换到 TA parser

### 目标

把 `load_module_ta` 中的两处 `vm.parse_source(content)` 替换为 `tokenizer.tokenize(content) + parser.parse(toks)`。

### 当前诊断结论

做了 swap 的版本（`/tmp/driver_old_swap.ta`）编译时报错：

```
compile error: unresolved symbol 'parse_ident_or_call'
compile error: unresolved symbol 'parse_ident_or_call'
compile error: unresolved symbol 'parse_braced'
compile error: unresolved symbol 'parse_param_list'
compile error: unresolved symbol 'parse_braced'
compile error: unresolved symbol 'parse_braced'
compile error: unresolved symbol 'parser.parse'
compile error: unresolved symbol 'parser.parse'
compile error: unresolved symbol 'parser.parse'
compile error: unresolved symbol 'parser.parse'
```

即 parser 模块的内部函数（`parse_ident_or_call`、`parse_braced`、`parse_param_list`、`parse`）在 `fn_names` 中不存在。

**已知事实**：
- 当前 `driver.ta`（用 `vm.parse_source`）编译无任何错误，fixpoint 收敛
- 单独写测试文件 `import parser` + `parser.parse(...)` 能正常工作
- `tokenizer.tokenize` 没有报错（tokenizer 的函数在 fn_names 中）
- 两个文件只有 `load_module_ta` 内部的 4 行差异，顶层 `import` 完全一致

**关键矛盾**：两份文件有完全相同的 `import parser`，import 解析应该把同样的 parser forms 拼入 AST。但 swap 版本中 parser 函数没注册上。

**待验证的假设**：
1. 路径差异导致文件读取失败，import 被静默跳过
2. pass1_register 在处理大量 forms 时出问题（codegen/typecheck 模块也很大）
3. 编译 `load_module_ta` 时有某种状态污染

### 执行步骤

- [ ] **Step 1: 精确复现并加诊断** — 给 `load_module_ta` 加一行打印 `list_length(forms)`，确认 parser 模块的 forms 是否被正确加载
- [ ] **Step 2: 定位根因** — 验证/排除上述三个假设，找到 `parse` 不在 `fn_names` 的具体原因
- [ ] **Step 3: 修复** — 基于根因修复代码（可能是 codegen.ta、parser.ta、或 driver.ta）
- [ ] **Step 4: 验证** — swap 版本编译成功，fixpoint 收敛，所有测试通过

## 后续阶段

### Phase 3 - 清理 C reader 代码
### Phase 4 - 完全 TA 自举