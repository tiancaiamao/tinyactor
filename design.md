# Design: 全仓 TA 覆盖率与源码选择器

## 现状

`make coverage-ta` 在 `Makefile` 中仅执行一次 `tinyactor build --cov lib/bootstrap/driver.ta`，所以该 covmap 只包含 bootstrap driver 的递归 import 闭包。测试通过 `TA_BOOTSTRAP` 使用这个带覆盖率的编译器，但普通测试程序由 `tinyactor run` 编译时没有传 `--cov`。因此测试程序、标准库和其它 repo `.ta` 文件都没有计入当前覆盖率。

`src/cov.c` 在启用 `TA_COV_DUMP_DIR` 后，每个进程退出时写入一份按 PID 命名的 ID 计数文件。`tools/coverage_ta.py` 用一个 covmap 合并所有 dump；若收集多个独立编译产物，coverage ID 会从 0 重用，不能直接把它们混在一个 dump 目录下与多个 map 合并。`tools/coverage_html.ta` 将 map 文件路径渲染成连续 HTML 源码段，目前没有文件导航控件。

## 目标与边界

- `make coverage-ta` 报告全仓库受版本控制/工作树内的 `.ta` 源码，而不只 bootstrap 源码。
- 测试期间实际执行到的行显示命中；已纳入仓库但没有覆盖数据的源码也出现在报告中并显示未覆盖。
- HTML 顶部提供可操作的源码选择器，一次展示所选文件的源码与命中情况。
- 不改变普通 `tinyactor` 使用时的覆盖率行为；coverage 仍只在 coverage target 运行时启用。

## 关键设计决策

1. **收集口径：repo 内所有 `.ta` 文件都进入报告。** 测试实际编译的程序及其导入源码携带运行命中数据；其余源码通过静态源码清单作为零命中项加入。这样“全仓”不依赖当前测试刚好 import 到每个模块。
2. **基于独立构建映射收集，而非直接拼接 ID 空间。** 每次 coverage 编译生成一个 covmap，测试执行后的 dump 必须与对应 map 关联，再由聚合器按源码路径/行/函数名合并。不能把独立编译的原始 ID 直接聚合，因为 ID 在各 map 内局部编号。
3. **控件用原生 `<select>` + 少量内联 JS。** 保持报告单文件、离线可查看；选择文件时切换对应源码 section，不引入前端依赖。

## 数据流与改动范围

1. `make coverage-ta` 创建唯一 run 目录，并生成全仓 `.ta` 清单。
2. coverage 测试构建以 coverage 模式编译 TA 源码。每个编译产物的 covmap 与其运行 dump 通过显式构建标识关联；不允许依赖非确定的 PID 顺序或跨 map ID 相同。
3. 聚合器读取多个 map/dump 组，按规范化源码路径、行号和函数名归并命中次数；清单中的每个 `.ta` 也进入 HTML，即使没有 map 项。无插桩数据的可执行语句/函数以 miss 显示。
4. HTML 接收仓库源码清单，生成文件选择器与对应的源码区段。默认选择首个文件；切换时隐藏其它区段。报告摘要与 miss 列表基于合并后的数据。

预计修改：`Makefile`、`tinyactor`（coverage 构建/运行时把 map 与 dump 关联）、`test/lib.sh`（coverage 模式下的测试构建路径）、`tools/coverage_ta.py`（多 map/dump 关联与清单补全）、`tools/coverage_html.ta`（文件选择器及清单渲染）、相关测试脚本与 coverage 文档。具体接口以实现前的 failing test 为准。

## 验收场景

### P0: 全仓覆盖数据
1. 执行 `make coverage-ta` 后，HTML 有仓库中所有 `.ta` 文件的源码段，不限于 `lib/bootstrap/`。
2. 一个普通测试实际运行的 TA 函数/语句显示命中；一个不被测试导入或调用的 repo `.ta` 源码也出现，执行代码显示未命中。
3. 来自不同独立编译产物、但 coverage ID 相同的行，其命中数据不会串到彼此。
4. HTML 和文本报告使用相同的合并结果；原有 coverage 测试仍然通过。

### P0: 源码选择器
1. 打开 HTML 默认看到一个源码文件；选择下拉项后仅显示对应文件源码及其 coverage。
2. 不依赖联网、服务端或外部 JS/CSS 资源。

## 边界条件与风险

- 执行器会并行跑多个测试进程；map/dump 配对必须抗 PID 并行和重复 ID。
- 负例测试编译失败也可能产生 map，但没有 runtime dump；其源码仍由全仓清单纳入，显示未覆盖。
- `tinyactor run` 使用临时构建产物，coverage 元数据需在构建与 VM dump 之间保留，且 cleanup 不能误删正在被消费的 map。
- 全 repo `.ta` 包含测试 fixture、示例和辅助工具；验收将它们都视为报告范围。若 `.ta` 文件无法被编译，是未覆盖源码而非 target 失败。
- repo 外临时测试源码不自动进入全仓清单；其编译 map 若在 coverage 运行中产生，可按 map 正常纳入。

## 验证

- 为源码选择器增加 HTML smoke test，断言 selector 存在、所有文件选项存在、切换脚本/隐藏行为存在。
- 为多 map 汇总增加独立 ID 冲突测试，并覆盖零命中清单文件。
- 运行 `test/run_cli_tests.sh`、coverage 聚合器测试及 `make coverage-ta`；按项目规则 `make fmt`，并检查 bootstrap fixed point（若触及 `lib/*.ta`）。