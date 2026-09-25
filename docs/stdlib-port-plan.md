# 标准库补全计划（Port Plan）v9

> 修订记录：v5 贯穿约定/dict定案/批次修订；v6 按源码核实信号 ABI、句柄 ABI、
> process/tls 边界；v7 闭合 R3 遗留——bufio 读函数注入、net.ta 错误载荷、
> process 注册表形状、c-module.md 同步修订；v8 闭合 R4 遗留——bufio v2
> 错误传播形态、process 管道非阻塞要求；v9 闭合 R5 边界——read_exact
> 残留语义、管道非阻塞仅限父进程端。

## 哲学

1. **语言层冻结**：语法 / tokenizer / parser / typecheck / VM opcode 不动。
2. **C 模块层是主力设施层**：有源码的 C 都跨平台，性能与底层设施靠 C 补齐；
   新设施 = `lib/xxx.c` + dylib + `import`，与 str/file/net 同级。
3. **TA 层做组合与糖**，API 风格抄 Gleam。
4. **规模双轨**：核心库（本 repo）= Janet 级封顶；其余照样要做，放三方包
   （独立 repo），C 模块机制即包机制，核心保证机制好用 + 提供模板。
5. **提取优先于凭空设计**（Go 标准库哲学）：真实使用中遇到的、属基础共性的
   需求，提取成库。样本见"提取管线"节。

参考系：Gleam（API 形状 + dict 设计）、Go（模块划分）、Erlang（timer/OTP）、
MoonBit core（buffer/encoding）、moonbitlang/x（fs/encoding/uuid）、Janet（核心规模标尺）。

---

## 贯穿约定（v5 新增，先于一切模块）

### 错误处理约定
- **C 原语层信号词汇表（按 net.c 真实 ABI，全库统一）**：
  | 信号 | 含义 |
  |---|---|
  | `nil` | **已挂起等待，恢复后需重试**（非错误；C 侧已 watch_fd + yield，
    actor 让出调度） |
  | `-1` | 硬错误（syscall errno / 参数非法） |
  | `'eof` | 流结束（仅读路径） |
  | symbol | 可区分失败（仅 connect 类多阶段操作：`'dns_error`/`'refused`/
    `'timeout`/`'error`） |
  | 正常值 | 成功 |
- **TA 包装层**：统一 lift——`nil` 在 TA 层循环重发（对用户不可见）、
  `'eof` → `Option.None`、`-1`/symbol → `Err(msg)`。net.ta 的映射见其条目。
  低频例外：`print` 等无失败语义的不包。
- 新 C 模块一律遵守此词汇表：C 出原子信号，TA 出类型化 API。
- **同步义务**：`docs/c-module.md` 现仍把 `nil` 写成通用失败值（§1/§4），
  与本词汇表冲突——**batch 0 必须重写其错误约定节**，否则照旧文档实现的
  新模块会把挂起误报成失败。
- **错误载荷定案**：C 的 `-1` 不携带 errno——batch 4 增补 `net.errno() -> int`
  （proc-local last errno，net_* 硬错误路径写入；几行 C，无语言改动）。
  net.ta 错误类型：`type NetErr { IoErr(errno : int); ConnErr(reason) }`——
  IoErr 携带 `net.errno()` 取回的码，ConnErr 承接 connect 的 symbol。
- **写语义定案**：C 层保持单次 `write(2)` 原子性；**net.ta.write 循环补写
  至全量**（full-write），返回 Result(int)=总字节数，部分写对用户不可见。

### C 可变资源句柄约定（v5 新增，原 blocker）
语言层无 opaque type / finalizer，故：
- **类型**：单构造 ADT 包装 int 句柄——`type Buffer { Buf(int) }`，纯语言
  现有特性，无语言层改动。
- **跨边界 ABI 定案**：TA 层 match 解包后，**C 一律收裸 int**（与现有
  net ABI 一致）；**防伪造交给 typecheck**（ADT 构造器即能力证明，运行时
  不做 tag 校验——保持 C 侧零感知，简单优先）。
- **失效句柄行为**：已 close 的句柄再操作 → C 按硬错误返回 `-1`，TA 层
  lift 成 `Err("closed")`；double close 幂等返回成功。注册表删除即失效。
- **生命周期**：显式 `close/free`，责任在调用方（Lua 哲学：fd 有进程退出
  兜底，malloc 型资源靠模块级注册表在 VM 退出时统一释放）。
- **actor 死亡**：v1 接受泄漏（文档标注每个模块的泄漏面），不引入 finalizer
  机制；真实泄漏出现时再议（追求"太简单以至于写不出 bug"）。
- **语义标注**：buffer/process/tls/sqlite/sdl = **可变句柄**（C 侧原地改）；
  str/list/dict = 值。可变句柄是对"TA 层函数式"的明确例外， confined 在
  C 边界后。

### 字符串语义
- str 现有约定按 codepoint 计数（length/char_at）；新增 C 函数沿用：
  trim/upper/lower/pad 等 v1 只做 ASCII 范围，行为写进文档；
  完整 Unicode 语义 = 三方包 `unicode`。

### 迭代语义
- v1 不做惰性流/迭代器：list 全量物化 + fold 已覆盖核心场景，理由是语言无
  宏/lazy 支撑，强行加抽象违背简单性。大文件场景用 bufio 行级 API 流式处理。
  真实性能瓶颈出现时再议。

### bufio 读源注入（v7 定案）
现有 `bufio.reader_new(fd)` 硬编码 `net.read`，TLS 句柄（`SSL*`，非 fd）
无法叠加。定案：**bufio v2 构造改收读函数**：`reader_new(read_fn)`，
`read_fn : fn(int) -> Val`（遵守信号词汇表）；保留
`reader_new_fd(fd) = reader_new(fn(n){ net.read(fd, n) })` 兼容糖。
tls.read/write 同构 ABI → 作 read_fn 直接注入。
**错误传播定案（v8/v9）**：现有 bufio 把 `-1` 当 EOF 吞错——v2 一并修正：
`-1` → `Err(IoErr(errno))`（经 net.errno()），`'eof` 且缓冲空 → `None`，
`'eof` 有残留 → `Ok(partial)`，正常 → `Ok(data)`；read_line/until/available
三个入口同构映射。**read_exact 例外（v9）**：EOF 未读满 n 时不准伪装成功
——返回 `Ok(None)`（缓冲保留已读部分），读满才 `Ok(Some(data))`；
随 net.ta 一起交付（batch 4）。

### 比较语义
- v1 不做通用 compare/深相等（语言 `<`/`==` 严格同型，不越层）。
- sort/min/max/uniq 收**显式比较器**版本：`sort(cmp, lst)`，提供
  `int_cmp`/`str_cmp`/`float_cmp` 基础件；dict 建树时传比较器（见下）。

---

## dict 设计（v5 定案）

- **结构定案：词序平衡树（AVL）**。理由：无需 hash（比较即可）、天然有序
  （keys 有序遍历，http header/json object 场景直接受益）、实现 ~100 行
  可读代码，bootstrap 调试成本可控。HAMT 需要 hash 基建，留作性能后手。
- **签名定案：`Dict(k, v)` 泛型，构造传比较器**：`dict.new(cmp) -> Dict(k,v)`
  ——语言无多态比较，比较器显式化正好贴合"比较语义"约定；提供
  `dict.str_new()` / `dict.int_new()` 便捷构造。
- 全程不可变：`insert(d,k,v) -> new_dict`，共享结构持久化。
- `set.ta` 薄封装。零 GC 集成成本。

---

## result/option 迁移（v5 定案）

- 构造器定名 `Ok(value)` / `Err(err)`（Gleam 风格；spec 示例的 `Error` 仅是
  示例不是承诺）。
- **一次性破坏式迁移**，无双轨：`json.try_parse` 切 Result，本 repo 测试同步
  更新；go.blog 侧 json（897 行 fork）在提取管线中归并进核心后改 import，
  旧 fork 删除。不做兼容层（语言未发布 1.0，破坏成本最低窗口就是现在）。

## mpc 归属（v5 定案）

- `mpc.ta`（lib/ 现有 438 行）**列为核心库正式成员**（解析基础设施，
  yaml/csv/md 的共同底座）；csv（batch 3）显式依赖它。
- go.blog 的副本在提取管线中删除，统一 import 核心。

---

## 核心库（本 repo）

### Workstream A — 数据与字符串

| 模块 | 抄 | 实现 | 内容 |
|---|---|---|---|
| `result`/`option` | Gleam | TA | Ok/Err、Some/None + map/bind/unwrap_or/or/try；json.try_parse 破坏式迁移 |
| `list` 扩充 | Gleam + Erlang | TA | filter/fold/zip/sort(cmp)/member/uniq/range/flat_map/chunk/keysort 等 |
| `str` 扩充 | Go strings + Gleam | **C** | split/join/trim/replace/upper/lower(ASCII)/pad/parse_float/进制/from_float |
| `buffer` | Go bytes + MoonBit buffer | C | **可变句柄**（见句柄约定）：new/push_str/push_byte/to_string/slice/close |
| `dict`+`set` | **Gleam** | **TA** | AVL 持久化 map，`dict.new(cmp)`，见定案节 |
| `encoding` | Go encoding/* + x/codec | C | base64(url-safe)/hex/percent；TA 层 lift Result |
| `strconv` | Go strconv | C+TA | int/float <-> string 统一收口、进制、宽度 |
| `math` 扩充 | Go math | C | floor/ceil/sqrt/pow/log/exp/三角/min/max/clamp |
| `mpc` | 已有 | TA | 正式列入核心；csv/yaml 的解析底座 |

### Workstream B — 文档格式解析

| 模块 | 实现 | 说明 |
|---|---|---|
| `json` v2 | TA（已有） | 序列化换 buffer、object 换 dict、错误带位置；try_parse 迁 Result |
| `markdown` | C **md4c** + TA | **go.blog 已落地**（vendored md4c.c + glue.c + md.ta 354 行）——
  提取而非新写；`parse(text) -> List[Node]` |
| `yaml` | C **libyaml** | TA 层收成与 json 同构 Value ADT，一套访问 API |
| `csv` | TA | 基于 mpc |

### Workstream C — 运行时设施

| 模块 | 抄 | 实现 | 内容 |
|---|---|---|---|
| `timer` ★ | Erlang | C+runtime | 最小堆定时器挂 scheduler：send_after / send_interval / cancel。**定案（user review #186）**：timer 是统一 poll 机制的一部分——sleep 必须 yield + 注册 timer、到期由 scheduler 唤醒，不许 nanosleep 阻塞 worker；net/timer/io 统一走同一个 poll loop（一个事件循环同时管 timers + fds），不许各模块自造等待。timer 落地时同步改造 time.sleep（TODO(poll) 已记在 lib/time.c） |
| `net` 非阻塞化 ★ | Go netpoll | C+runtime | **现状基线（全部已实现）**：listen/accept/read/write 均
  非阻塞 + EAGAIN→watch_fd+yield、三段式 connect（issue #29）、poller 带
  deadline。**真实 delta**：① 可复现多连接负载测试 + 无 actor 饥饿验收
  阈值；② 据负载测试决定 poll() 是否换 epoll/kqueue（可能不必，不预设）；
  ③ `net.ta` API 层（下条）。batch 4 规模因此显著小于 v4 表述 |
| `net.ta` ★ | — | TA | **新列交付物**。C ABI 逐函数签名与映射（照信号词汇表）：
  `connect(host, port, timeout_ms) : fd / nil(挂起重发) / Err(symbol)`；
  `read(conn, n) : Result(Option(bytes))`（nil 循环重发、`'eof`→None、
  `-1`→Err）；`write(conn, s) : Result(int)`；`listen/accept` 同构。
  句柄 `type TcpConn { Conn(int) }` + close。http/tls/process 均压在此 API 上 |
| `tls` ★ | — | C（系统 OpenSSL，备选 bearssl） | **边界定案**：TLS 模块拥有 `SSL*`（句柄注册表），底层 fd 来自
  TcpConn 解包或内部 connect；handshake 的 WANT_READ/WANT_WRITE 复用
  net 信号协议（watch_fd + yield + 返回 nil 挂起重试）；`tls.read/write`
  与 net.read/write 同构 ABI → 经 bufio 读源注入叠加（见 bufio 定案），
  **net.ta 零改动**；close 默认 TLS shutdown + 关底层 fd，可选只关 TLS。
  依赖 net.ta 定案 |
| `time` | Go time | C | now_ms / monotonic_ms / sleep |
| `os` + `fs` 扩充 | Go os | C | getenv/args/exit/hostname；list_dir/remove/rename/stat/cwd |
| `process` | Go os/exec | C | **句柄形状定案**：`type Proc { P(int) }`，int 是 **process 模块
  注册表下标**，表项 = {子进程 pid, stdin_fd, stdout_fd}（单 int 句柄约定
  因此成立）。**管道非阻塞要求（v9）**：仅父进程持有的两端 fcntl 设
  O_NONBLOCK（否则阻塞 read/write 挂死 worker，破坏信号协议）；dup2 给
  子进程的 stdio 端保持阻塞（同一 open-file description 会共享标志，
  普通子进程收到 EAGAIN 会误判错误）。取流：`process.stdin_w(p) / stdout_r(p) -> int fd`（裸 fd
  直接进 net.read/write 与 poller，遵守信号词汇表）。生命周期：close_stdin
  用 net.close(stdin_fd) → 读 stdout 至 `'eof` → `process.wait(p) -> int
  exit code`（收尸并从注册表摘除、close 剩余 fd）；`process.close(p)` =
  未 wait 时的清理路径（关双管道 + 收尸），幂等。失败（fork 失败）→ `-1` |
| `random` | Go math/rand | C | srand/rand_int/rand_float；TA：choice/shuffle/sample |

### Workstream D — 应用层

| 模块 | 抄 | 实现 | 内容 |
|---|---|---|---|
| `url` | Go net/url | TA+C | parse、query encode/decode（配 encoding） |
| `http` 补全 | Go net/http | TA | client 重定向/超时、header dict 化、serve 路由 dict 化 |
| `log` | Go log | TA | level + 时间戳（配 time）+ stderr |
| `arg` | Go flag | TA | --key=value、位置参数、usage |
| `path` | Go filepath | TA | join/dirname/basename/ext |
| `html` | go.blog 提取 | TA | 转义 + 标签构建（html.ta 273 行成熟实现） |
| actor 设施 | Erlang | TA | registry / supervisor / serve.ta→gen_server；只依赖
  spawn/send/monitor（VM 均已有），**不依赖 net**（v5 依 R1 前移） |
| `test` | MoonBit quickcheck | TA | 断言糖 + property testing（配 random） |

---

## 三方包路线图（独立 repo，确认要做）

| 包 | 来源/选型 | 依赖 | 批 |
|---|---|---|---|
| `sdl`（模板首发） | SDL2 C 封装（可变句柄约定适用） | 无 | T1 |
| `sqlite` | sqlite3 C 封装（同上） | 无 | T1 |
| `queue`/`deque`/`priority_queue` | Go container + MoonBit | core dict | T1 |
| `regexp` | 单文件 NFA 引擎（不引 PCRE 全家桶） | 无 | T2 |
| `decimal`/`rational` | moonbitlang/x/num 思路 | 无 | T2 |
| `unicode` | moonbitlang/x/unicode 思路（str UTF-8 完整语义） | 无 | T2 |
| `jwt`/`bcrypt` | moonbitlang/x | core encoding+json+random | T3 |
| `toml`/`json5`/`xml` | 按需 | core mpc/json | T3 |
| `curl`/`ssh` 等框架 client | 有源码的 C 库皆可包 | core net.ta | T3 |

## 提取管线（持续供给）

真实项目 → 基础共性需求 → 提取成库（先三方包，频繁使用再转正核心）。

样本 **~/project/go.blog**：md4c 胶水 + md.ta、template.ta、html.ta、core.ta。
**归属决策（v5）**：提取后 go.blog/src-ta/lib 五份 fork（json/mpc/html/md/
template）删除，改为 import 核心 lib——呼应 issue #67 教训（import 链接当前
源码），避免两边漂移使"已落地验证"失效。

每个真实项目收尾过一遍：这次手写了什么本该 `import` 的东西？提取一个包。

---

## 顺序（依赖驱动，v5 修订）

| 批 | 内容 | 依赖 |
|---|---|---|
| 0 | **贯穿约定落地**：result/option 破坏式迁移 + 句柄约定 + **信号词汇表
  重写 docs/c-module.md 错误约定节**（消除 nil 语义冲突） | 无 |
| 1 | list + str + buffer（句柄首个实践）+ math + time + os/fs + path | batch 0 |
| 2 | **dict/set（AVL）** + encoding + random + strconv + url | batch 1 |
| 3 | timer + process + log + arg + csv + json v2 + **actor 设施（自 net 解绑）** | batch 1–2 |
| 4 | **net 非阻塞化 delta + net.ta + net.errno() + bufio v2 读源注入**（旗舰，独立分支） | timer |
| 5 | tls + http 补全 | net.ta |
| 6 | markdown（提取）+ yaml（libyaml）+ template（提取）+ html（提取）+ test | batch 2 |
| 7 | 三方包模板（sdl/sqlite）+ 包机制文档 + T1 包 | 核心稳定 |

每个 PR：feature branch → `make bootstrap` ×2 fixed point（TA 层改动）→
`make test` 0 failures → `make fmt`。纯 C 模块不触发 bootstrap，但配 C 测试 +
`test/basic/*.ta` 运行时正例。md4c/libyaml/openssl 走系统链接或 vendored，
Makefile 加开关。

## 流程约定（user review #186/#187 后追加）

- **类型签名尽量写全**：ta 是有类型的语言，lib/*.ta 公开函数一律显式标注
  参数与返回类型，让调用方在编译期拦住类型错误。不许为了省事用无类型参数
  （external C 声明除外——那是 ABI 边界）。
- **并发用 git worktree**：多个 PR 互不阻塞时，用 `git worktree add` 各自
  独立目录 + 独立分支推进，避免单工作目录里 checkout 串行等待。
- **CI 格式化版本注意**：本地 clang-format 18 与 CI 23 对个别构造（cast 后
  跟 `new->…`）裁定相反；写 C 时避免该构造，fmt-check 以 CI 为准。

规模标尺：核心 batch 0–6 ≈ Janet 核心同级 + 文档解析面齐；
三方包路线图保证长期供给。