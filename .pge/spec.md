# 类型系统增强 Spec

## 已完成 (Phase 1-5)
- 泛型 ADT 支持 (t_app, collect_variants)
- proc_push 内存损坏修复
- typecheck 非 pair AST 崩溃修复
- typecheck 错误报告 (unify_check, --check 模式)
- 性能优化 (level-based generalization, O(N³)→O(N²T))
- 测试覆盖 + 性能分析文档

## Phase 6: 类型系统健壮性增强

### A6: type-sig 注解不再崩溃
- `type-sig name (types) ret_type` 声明后 typecheck 不 segfault
- type-sig 指定的类型被正确注册并与推断结果统一
- Verify: `type-sig my_fn (int) int` + `fn my_fn(x) { x + 1 }` 通过 --check 无错误
- Verify: `type-sig must_str () string` + `fn must_str() { 42 }` 报告类型错误

### A7: 错误信息包含函数名位置
- 每条类型错误包含出错函数名
- 格式: `in function 'foo': cannot unify int with string`
- Verify: 在有类型错误的测试文件上 --check 输出包含函数名

### A8: 更多内建函数类型签名
- 补全常用的 str.*、math.* 等函数签名到 make_builtin_env
- Verify: 调用 str.from_int 等不再因 unbound 被放过

## 约束
- 不改 VM（无新操作码）
- typecheck 仍是宽容模式（错误为警告，不阻止编译）
- 向后兼容