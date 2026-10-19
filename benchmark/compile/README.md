# benchmark/compile — 编译时间基准（issue #258）

`benchmark/` 下的编译时间基准，与同级的运行时基准（`core/`、`actor/`、`gc/`）
并列，互不干扰。口径照抄 [langcompilebench](https://github.com/lpil/langcompilebench)：

- **100 个源文件 × 每文件 100 个平凡函数**（`pub fn helloK() { "worldK" }`），
  合计 1 万函数；`src/main.ta` import 全部并逐个调用（避免整体被裁剪）。
- 每次构建**先清 modsig 签名缓存**（对应 `go clean -cache`），再
  `tinyactor build src/main.ta out/main.tabc` 完整编译，bash `time` 计时。
- `run.sh` 跑 **11 次取中位数**（sort 后第 6 小），每次间隔 2s。

测的是纯编译前端吞吐（tokenize → parse → typecheck → codegen + 模块解析），
不含运行时、不含增量构建。1 万个小函数刻意放大固定开销。

## 用法

```sh
make            # 仓库根：tavm + lib 模块是基准的被测对象
./benchmark/compile/run.sh          # cold：每次清缓存（cache miss，正典口径）
./benchmark/compile/run.sh warm     # warm：缓存全程保留（cache hit）
./benchmark/compile/run.sh ratio    # 11 组 cold→warm 交错配对：两个中位数 +
                                    # 每对比值的中位数 = modsig 缓存收益
```

单独一次计时：`./benchmark/compile/build.sh [--keep-cache]`。
重新生成语料：`./benchmark/compile/gen.sh [files] [fns-per-file]`（默认 `100 100`）。

## 规模上限

canonical 语料 **100 文件 × 100 函数** 现在 cold 也能在默认 64MB actor
arena 下编译通过，直接跑即可，不再需要缩小语料：

```sh
./benchmark/compile/run.sh          # cold：每次清缓存
./benchmark/compile/run.sh ratio
```

根因（#308 修复前 ≥20×100 确定性 `actor heap arena exhausted` abort）：
`compile.ta` 的 `flatten_fns` 是**非尾递归**，把每个定义处随定义数增长的
常量池副本各压一层栈帧 —— N 个定义 = O(N²) 对驻留，大语料下撑爆 arena。
改为尾递归 + 逆序累积后驻留回到 O(N)。

## 口径细节

- 签名缓存固定在 `benchmark/compile/.build/`（`TA_SIG_CACHE_DIR`），
  不碰仓库根的共享 `.build/modules`——基准自包含、可重复。
- `src/*.ta`、`out/`、`.build/` 全部生成、不入库（本目录 `.gitignore`）；
  提交的是 `gen.sh` / `build.sh` / `run.sh`。
- 结果**只跟自己比**（typecheck/parser 优化前后、缓存开关）；跨机器对比
  无意义，`run.sh` 表头打印机器型号与 commit 以便归档。
- 未来变体（预留，未实现）：单文件 1 万函数（纯前端吞吐）vs 100 文件
  （含模块解析开销）。