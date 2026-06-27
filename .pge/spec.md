# Spec: TinyActor 多层自举架构

## Goal

将 TinyActor 的编译器从 C 双份实现（compile.c + codegen.ta）重构为三层自举架构：.ta 层 → Lisp IR 层 → bytecode，每层用自己语言实现，消除重复。

## Acceptance Criteria

### Phase 1: codegen.lisp 基础版（追平 codegen.ta）

- [ ] codegen.lisp 文件存在，用 S-expr 语法实现 — Verify: `test -f lib/codegen.lisp`
- [ ] codegen.lisp 能被 reader.c + compile.c 编译 — Verify: `./tinyactor lib/codegen.lisp` 不报错
- [ ] codegen.lisp 能编译简单 .lisp 程序产出正确 .tabc — Verify: 写一个 test.lisp，用 codegen.lisp 编译后加载执行，输出正确

### Phase 2: codegen.lisp 补齐功能

- [ ] codegen.lisp 支持 and/or 短路求值 — Verify: `(and (= 1 1) (= 2 2))` 编译后执行返回 true
- [ ] codegen.lisp 支持 lambda + closure（free var 分析）— Verify: closure-curry.lisp 测试通过
- [ ] codegen.lisp 支持 let 多绑定 — Verify: `(let ((a 1) (b 2)) (+ a b))` 编译执行返回 3
- [ ] codegen.lisp 支持 spawn/send/recv/self/monitor — Verify: ping_pong.lisp 测试通过
- [ ] codegen.lisp 支持 tail call — Verify: 尾递归 fib 不溢出
- [ ] codegen.lisp 支持 receive-scan 特殊形式 — Verify: recv-scan.lisp 测试通过
- [ ] compile.c 也补齐 and/or 支持（bootstrap 路径需要）— Verify: `make && bash test/run_all_tests.sh` 全通过

### Phase 3: parser.ta pattern desugar

- [ ] parser.ta 的 parse_match 产出 desugar 后的 if/let（不再产出 match AST）— Verify: grep parser.ta 无 'match 输出
- [ ] parse_receive 产出 receive-scan 形式 — Verify: grep parser.ta 无旧式 receive AST
- [ ] 所有 .ta 测试通过（match 相关）— Verify: adt-basic.ta, echo_test.ta, exhaustiveness.ta, recv-scan.ta 测试通过

### Phase 4: VM 多模块加载

- [ ] vm_load_module 支持多模块追加（code/fn_table/symbols rebase）— Verify: 连续加载两个 .tabc 不崩溃
- [ ] vm_load_bytecode 从内存 buffer 加载字节码 — Verify: C 函数存在且可调用

### Phase 5: bootstrap driver

- [ ] driver.lisp 存在，编排编译管线 — Verify: `test -f lib/driver.lisp`
- [ ] main.c 精简为加载 .tabc + spawn driver — Verify: `make` 成功

### Phase 6: C 编译器降级

- [ ] compile.c / reader_ta.c 标记为 bootstrap-only — Verify: 注释标明，不影响 runtime

## Constraints

- 语言完全不可变（design.md），不支持 set!
- 不改 VM 字节码格式（Phase 7 可选的 MATCH_* 移除除外）
- 不改 NaN-boxing 值表示
- 60 个现有测试全通过
- 不引入 set! 或可变变量

## Out of Scope

- 热更新实现（远期参考 BEAM）
- compile.c / reader_ta.c 彻底删除（等自举稳定后）
- 类型系统（远期 .ta 层）
- MATCH_* 指令从 VM 移除（Phase 7 可选）