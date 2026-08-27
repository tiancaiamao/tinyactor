# kernfuzz-facts — 首日语言事实钉死清单（实测）

task-ast-dump 交付物 2。每条 = 结论一句话 + 验证命令 + 输出摘录 + 与设计文档假设的一致性。
样例文件在 `/tmp/kf-facts/`，命令均于仓库根目录执行。实测日期：2026-08-26（接手补全）。

## f1 — `==` 对 pair/list 的语义

**结论：引用比较（identity），不是深比较。** 同构 pair 恒不相等，别名绑定才相等。
golden 复刻 `==` 时必须用 pointer/identity 语义，不能按结构相等实现。

验证命令：

```
./tinyactor run /tmp/kf-facts/f1-eq-pair.ta
```

输出摘录（5 行，对应样例中 5 个 `print`）：

```
false    ; [1, 2] == [1, 2]            —— 字面量同构，不等
false    ; let a=[1]; let b=[1]; a==b  —— 独立构造，不等
false    ; cons(1,cons(2,nil)) 两份     —— 嵌套同构，不等
false    ; 嵌套 pair 同构两份            —— 不等
true     ; let c=a; c==a               —— 同一对象的两个绑定，相等
```

与设计文档假设的关系：§5.0 表「`==` 对 pair/list 的语义实现首日实测钉死」→ 已钉死：
**identity 语义**。

## f2 — rest 模式语法 `[h, ..t]`

**结论：不被 parser 接受。** `[h, ..t]` 触发 `parse error: unexpected character`
（`..` 是未知字符，token 级失败），`tinyactor build` / `run` 均 exit 1。
等价 rest 语法是 `cons(h, t)` 构造器模式（头 + 尾绑定，语义等价于 rest）。
**生成器不得产出 `[h, ..t]`；rest 场景一律渲染为 `cons(h, t)`。**

验证命令：

```
./tinyactor run /tmp/kf-facts/f2-rest-pattern.ta   # 使用 [h, ..t]
./tinyactor run /tmp/kf-facts/f2b-cons-pattern.ta  # 使用 cons(h, t)
```

输出摘录：

```
# f2-rest-pattern.ta（[h, ..t]）：
/tmp/kf-facts/f2-rest-pattern.ta: parse error: unexpected character (token 14)
error: build failed - no output produced (/tmp/tinyactor-rMJTZK.tabc)
（exit 1）

# f2b-cons-pattern.ta（cons(h, t)）：
7     ; first([7, 8, 9]) —— h 绑定到首元素
0     ; first([])        —— nil 臂
3     ; len([7, 8, 9])   —— 递归 rest 正常工作
（exit 0）
```

与设计文档假设的关系：R3 评审怀疑「spec 模式表未收录 `..t`」→ **怀疑成立，实测确认
不支持**。§5.0 表「rest 模式语法实现首日以 parser 实测为准」→ 已钉死：不存在，用
`cons(h, t)`。另注：§5.0「最小完整示例程序」原样含 `[h, ..t]`，**原样 build 必失败**
（见 `/tmp/kf-facts/sample-original-fails.ta` 存档）；重建版见 `/tmp/kf-facts/sample.ta`。

## f3 — 顶层互递归可见性

**结论：顶层两个 fn 互相调用可以正常编译并运行。** 生成器可产出顶层互递归结构。

验证命令：

```
./tinyactor run /tmp/kf-facts/f3-mutual-rec.ta   # is_even / is_odd 互相调用
```

输出摘录：

```
true    ; is_even(10)
true    ; is_odd(7)
（exit 0）
```

与设计文档假设的关系：§5.0 作用域规则「顶层定义间互递归是否可见：实现首日核实」
→ 已钉死：**可见，无限制**。

## f4 — main 返回值是否打印

**结论：不打印。** `fn main() -> int { 42 }` 的 stdout 完全为空，exit 0。
main 的返回值被 VM 丢弃（或被 CLI 当作退出信息但不上 stdout）。

验证命令：

```
./tinyactor run /tmp/kf-facts/f4-main-return.ta
./tinyactor run /tmp/kf-facts/f4-main-return.ta 2>/dev/null | od -c   # 确认 stdout 为空
```

输出摘录：

```
（无任何输出，od -c 显示空流；exit 0）
```

与设计文档假设的关系：与 §1.1「print 是唯一副作用、输出格式」一致——非 print 值不出现在
stdout。golden 比对时 main 的返回值不应进入期望输出。

## f5 — `file.read` 存在性与签名

**结论：存在，签名 `file.read(path) -> String | nil`（1 参，文件不可读返回 nil）。**
C 模块函数，非 TA 库函数。

验证命令（引用源码）：

```
sed -n '4p' src/file.c          # 头注释：file.read(path) -> String | nil (whole file as string)
sed -n '151p' src/file.c        # 注册：{"read", file_read, 1}
sed -n '571p' lib/driver.ta     # 现存用法：let src = file.read(path)
```

引用行（`lib/driver.ta:571`，`load_source_ta` 内）：

```ta
let src = file.read(path)
```

与设计文档假设的关系：一致（§5.3 / task 定义要求先核实再使用；ast-dump.ta 即基于此
实现，未凭空猜函数名）。

## f6 — `tinyactor build` 输出路径语法与失败 exit code

**结论：输出路径是位置参数 `tinyactor build <src>.ta [<out>.tabc]`（缺省
`<src>.tabc`），没有 `-o` 标志。** 传 `-o` 会被当成源码路径，报
`error: TinyActor source file not found: .../-o`，exit 1。
build 失败（parser 错误）→ **exit 1**，不产生产物；`tinyactor run` 对 build 失败
同样 **exit 1**。设计文档 §5.4 调用卡的 `-o` 写法是错的。

验证命令与输出摘录：

```
# 位置参数（正确形态）：
$ ./tinyactor build /tmp/kf-facts/f4-main-return.ta /tmp/kf-facts/f4-pos.tabc
exit=0，产物 /tmp/kf-facts/f4-pos.tabc 生成（412 字节）

# -o 标志（错误形态，被当源码路径）：
$ ./tinyactor build -o /tmp/kf-facts/f4-o.tabc /tmp/kf-facts/f4-main-return.ta
error: TinyActor source file not found: /Users/genius/project/tinyactor/-o
exit=1

# build 失败（parser 错误）：
$ ./tinyactor build /tmp/kf-facts/f2-rest-pattern.ta /tmp/kf-facts/f2.tabc
/tmp/kf-facts/f2-rest-pattern.ta: parse error: unexpected character (token 14)
error: build failed - no output produced (/tmp/kf-facts/f2.tabc)
exit=1，产物不存在

# run 对 build 失败的 exit code：
$ ./tinyactor run /tmp/kf-facts/f2-rest-pattern.ta
（同上两条错误行）
exit=1
```

与设计文档假设的关系：exit code 断言（build 失败 1、run 对 build 失败 1）**一致**；
但 §5.4 调用卡的 `-o` 标志**不一致**（见文末修正项 1）。

## f7 — 零参匿名闭包两形态

**结论：`fn{..}` 与 `fn(){..}` 均可编译运行，行为一致。** 生成器可自由产出两种形态
（T12 包装所需显式空参形态可用）。

验证命令：

```
./tinyactor run /tmp/kf-facts/f7-zero-closure.ta
```

输出摘录：

```
111    ; let f1 = fn { 111 };        f1()
222    ; let f2 = fn() { 222 };      f2()
42     ; apply(fn(z: int) -> int { z * 2 }, 21)  —— 带参闭包经函数值传递调用
（exit 0）
```

与设计文档假设的关系：一致（§1.1「fn 两形态……二者 AST 同构」，§5.0 三形态全入子集）。

## 补充 — divzero 死亡协议（推翻 §1.1「stderr 静默」）

**结论：main 进程 divzero 死亡时，stdout 保持干净（死亡前的 print 完整落盘、自带换行），
stderr 会输出死因行 `** CRASH pid 1: 'divzero` + `at main (fn 0)`，exit 1。**
死因**可以**从进程外部（stderr）观测——§1.1「全部 exit code 1、stderr 静默，死因不可
从进程外部观测」的表述被推翻。

验证命令：

```
./tinyactor run /tmp/kf-facts/f6b-divzero.ta 1>/dev/null 2>/tmp/kf-facts/f6b-stderr.txt
./tinyactor run /tmp/kf-facts/f6b-divzero.ta 2>/dev/null   # 只看 stdout
```

输出摘录：

```
# stdout（2>/dev/null）：
MARKER-BEFORE-DEATH
# stderr（1>/dev/null 捕获）：
** CRASH pid 1: 'divzero
   at main (fn 0)
# exit code：1
```

对工具链的影响：§5.1.3 比对协议若依赖「stderr 静默」区分结局需改为：build 失败与
divzero 死亡都可从 exit 1 + stderr 内容区分（build 失败 stderr 含
`error: build failed - no output produced`；divzero 死亡 stderr 含
`** CRASH pid 1: 'divzero`）。

## 设计文档需修正项

1. **§5.4 调用卡 `build src -o <path>`**：`tinyactor build` 无 `-o` 标志，正确形态是
   位置参数 `tinyactor build <src>.ta [<out>.tabc]`（缺省 `<src>.tabc`）。见 f6。
2. **§1.1「stderr 静默，死因不可从进程外部观测」**：实测 main 进程 divzero 死亡时
   stderr 输出 `** CRASH pid 1: 'divzero` + 位置行，死因外部可观测；exit 1 断言不变。
   见 f6b / 补充条目。

（另注，不列入上两条但工具链必须知道：§5.0「最小完整示例程序」原样含 `[h, ..t]`，
build 必失败；等价重建版（`cons(h, t)` + ADT/guard）已落盘 `/tmp/kf-facts/sample.ta`
并 build 通过。见 f2。）

## 宿主语言变更记录（2026-08-27，用户决策）

- 工具链宿主语言从 **Scheme/Guile** 改为 **Python 3（仅标准库）**（见设计文档 DEC-6 / §5.3）。
- 理由：AI 可用性、Python 原生任意精度整数（int48 归一不需要手动 fixnum 双截断规避）、
  subprocess 标准件、无 Guile 安装依赖。
- s-expr 中间表示（AST dump）不变，仍由 `ast-dump.ta`（真实 parser）产出。
- `golden/interp.scm` / `test-interp-core.scm` 保留，作为 Scheme 语义基准；
  Python 版（`golden/golden.py` / `test_golden.py`）须与之一致（w48/算术语义/match）。
