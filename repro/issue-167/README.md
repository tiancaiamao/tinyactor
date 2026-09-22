# Issue #167 复现材料归档

自宿主编译器 heisenbug：codegen 内新 helper 函数 let 绑定 TA 调用结果 →
自举链 gen2 确定性崩溃（car: expected pair, got tag=0xff04 / 值串扰）。
详见 issue #167。

## 文件
- `codegen-deltaA.ta` … `codegen-deltaL.ta` — 二分序列：main 版 codegen.ta
  的逐级修改版（每份相对上一份只改一个变量）。开发会话中的结论：
  - deltaC/D 形态（compile_pattern int 分支抽 helper，内含 let 绑定
    compile_expr）→ gen2 崩溃
  - "定义 helper 但不调用"（deltaE）/ 纯内联（deltaD 之前形态）→ 不崩
  - 最终 #166 采用完全内联 + 叶子绑定形态，链验证通过
  - **注意**：该结论与"main 现有代码大量 let 绑定 TA 调用却正常"矛盾，
    二分变量未必抓对，需重查
- `dis.py` — bytecode 反汇编器（`python3 dis.py file.tabc start end`），
  定位 gen2 崩溃时发现 fail-jump 未回填（JUMP_IF_FALSE 0）用过

## 复现步骤（2025 年开发会话验证过的命令形态）
```bash
# 旧 tavm：从 main (0ee7962) 构建一次即可
make tavm   # 或 make；产物 ./tavm

# 链：gen1 = 旧 tavm + main bootstrap.tabc 编译 delta 版编译器源码
mkdir -p /tmp/m167/{old,gen1}
cp /Users/genius/project/tinyactor/lib/bootstrap.tabc /tmp/m167/gen0.tabc
cp -r lib /tmp/m167/old/lib   # 含被替换的 codegen.ta（delta 版）
# 用 deltaN.ta 覆盖 lib/bootstrap/codegen.ta 后：
./tavm /tmp/m167/gen0.tabc lib/bootstrap/driver.ta /tmp/m167/gen1/bootstrap.tabc

# gen2 = gen1 编译器编译 main 源码 —— 崩溃点
cd /tmp/m167/gen1
cp -r /Users/genius/project/tinyactor/lib ./lib
cp /tmp/m167/old/tavm ./tavm   # gen1 需旧 tavm 跑（旧 opcode 编号）
cp /Users/genius/project/tinyactor/lib/bootstrap/driver.ta /dev/null # (示意)
```
（实际构建脚本形态见 issue #167 正文与开发会话；核心是
old-tavm + gen0(main bootstrap) → gen1(delta 源码) →
gen1-tavm + main 源码 → gen2，崩溃发生在 gen2 编译 driver.ta 时。）
