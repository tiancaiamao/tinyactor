// docs/tour-lessons.js — TinyActor 交互式教程课程数据（26 课）
//
// 每个 LESSONS 元素：
//   id       — 唯一 id（kebab-case）
//   section  — 分组名（Basics | Functions | Flow control | Data types |
//              Type system | Actors | Modules）
//   title    — 课程标题
//   text     — 概念讲解（中文，2-4 句）
//   code     — 完整的 TA 程序（可在 WASM 编译 + 运行；要跑 import 的课
//              需要模块树，编译时按 <root>/vendor/<树>/<路径>.ta 解析）
//
// 写作规矩（改课必读）：
//   - cons 只表达**列表**（cdr 必须是 nil 或另一个 cons）；点对、记录这类
//     定长数据用 ADT（Point(1, 2)，运行时是带标签元组 tuple Point 1 2）。
//     语言的后续方向就是禁掉 cons 的 pair 用法，课里别再示范。
//   - match 的例子优先用 ADT 构造子模式，不要拿 pair 当解构示范。
//   expected — 预期输出（每行一条，与真实 stdout 逐行一致）。进程崩溃时
//              VM 会打 `** CRASH …` 报告，那是运行时细节，不计入 expected
//
// 本文件由 `node scripts/verify-lessons.mjs`（`make test-tour`）逐课编译 +
// 运行并比对 expected：`const LESSONS = [ ... ];` 之间是严格 JSON，数组内
// 不含注释/尾逗号，可直接 JSON.parse 提取。改课之后跑一遍。

const LESSONS = [
  {
    "id": "hello-tinyactor",
    "section": "Basics",
    "title": "Hello, TinyActor",
    "text": "TinyActor（简称 TA）是一门函数式、类型安全的 actor 语言。main 是程序入口，print 把值输出到控制台。每个程序都从一个 main 函数开始执行，它在自己的进程里运行。",
    "code": "fn main() {\n  print(\"hello\")\n}",
    "expected": "hello"
  },
  {
    "id": "value-types",
    "section": "Basics",
    "title": "值类型",
    "text": "TA 的值类型有整数、浮点、字符串、布尔、符号、nil、列表（nil 结尾的 cons 链）、元组（ADT 构造子的底层表示）、闭包和 pid，全都不可变。注意 'a'（单引号包住一个字符）不是独立的字符类型，它只是整数的一种写法，打印出来是 ASCII 码 97；而 'done 是符号（symbol）。print 可以打印任意类型的值。",
    "code": "fn main() {\n  print(42)          // 整数\n  print(3.14)        // 浮点\n  print('a')         // 字符字面量 → 整数 ASCII 码 97\n  print(\"hi\")        // 字符串\n  print(true)        // 布尔\n  print('done)       // 符号\n}",
    "expected": "42\n3.14\n97\nhi\ntrue\ndone"
  },
  {
    "id": "let-binding",
    "section": "Basics",
    "title": "变量绑定",
    "text": "let 把表达式的结果绑定到名字，之后就可以引用它。TA 的变量是不可变的——绑定后不能再赋值。let 在函数体内按顺序执行，后面的绑定可以引用前面的绑定。",
    "code": "fn main() {\n  let x = 42\n  print(x)\n}",
    "expected": "42"
  },
  {
    "id": "operators",
    "section": "Basics",
    "title": "运算符",
    "text": "算术运算符 + - * / % 作用于整数，比较运算符 == != < <= > >= 返回布尔值。&& 和 || 是短路求值的逻辑运算。TA 没有逻辑非 ! 这个运算符：取反写 x == false，或者用标准库的 bool.not（需要 import \"tinyactor:lib/bool\"）。",
    "code": "fn main() {\n  let x = 3\n  let y = 4\n  print(x + y)         // 7\n  print(x * y)         // 12\n  print(y % x)         // 1\n  print(x < y)         // true\n  print(x != y)        // true\n  print(true && false) // false\n  print(false || true) // true\n  print(1 == 1)        // true\n}",
    "expected": "7\n12\n1\ntrue\ntrue\nfalse\ntrue\ntrue"
  },
  {
    "id": "block-expression",
    "section": "Basics",
    "title": "块表达式",
    "text": "花括号 { } 创建顺序执行的块，块的值是最后一个表达式的值。可以在块里用 let 组织中间的局部计算。块表达式让多步计算可以写成一个表达式。",
    "code": "fn main() {\n  let result = {\n    let x = 40\n    let y = 2\n    x + y\n  }\n  print(result)\n}",
    "expected": "42"
  },
  {
    "id": "function-definition",
    "section": "Functions",
    "title": "函数定义",
    "text": "fn name(args) { body } 定义命名函数，函数体最后一个表达式的值就是返回值。函数可以递归调用自己。经典例子：斐波那契数列，fib(20) 的结果是 6765。",
    "code": "fn fib(n) {\n  if n < 2 {\n    n\n  } else {\n    fib(n - 1) + fib(n - 2)\n  }\n}\n\nfn main() {\n  print(fib(20))\n}",
    "expected": "6765"
  },
  {
    "id": "anonymous-function",
    "section": "Functions",
    "title": "匿名函数",
    "text": "fn(x) { x + 1 } 创建匿名函数（lambda），可以赋值给变量或作为参数传递。带参数的匿名函数用 fn(x) { ... }，不带参数的用 fn { ... }。匿名函数会自动捕获外层变量形成闭包。",
    "code": "fn adder(n) {\n  fn(x) { x + n }\n}\n\nfn main() {\n  let g = adder(5)\n  print(g(3))\n}",
    "expected": "8"
  },
  {
    "id": "closures",
    "section": "Functions",
    "title": "闭包",
    "text": "闭包会捕获定义时的自由变量，即使外层函数已经返回，变量依然可用。这里三层嵌套的闭包分别捕获 x、y、z，最终计算 1 + (2 + 3) = 6。捕获发生在创建闭包的那一刻：自由变量按值复制进闭包（TA 的值不可变，所以看到的就是当时的那个值；buffer 这类可变句柄复制的是句柄，多个闭包仍指向同一块内存）。",
    "code": "fn f(x) {\n  fn(y) {\n    fn(z) {\n      x + (y + z)\n    }\n  }\n}\n\nfn main() {\n  let g = f(1)\n  let h = g(2)\n  print(h(3))\n}",
    "expected": "6"
  },
  {
    "id": "higher-order-functions",
    "section": "Functions",
    "title": "高阶函数",
    "text": "函数可以接收函数作为参数，也可以返回函数。map 把传入的函数应用到列表的每个元素，生成新列表。TA 的列表是 nil 结尾的 cons 链，用 cons / car / cdr / null? 操作。",
    "code": "fn map(f, lst) {\n  if null?(lst) {\n    nil\n  } else {\n    cons(f(car(lst)), map(f, cdr(lst)))\n  }\n}\n\nfn build(n) {\n  if n == 0 {\n    nil\n  } else {\n    cons(n, build(n - 1))\n  }\n}\n\nfn main() {\n  print(map(fn(x) { x + 1 }, build(5)))\n}",
    "expected": "(6 5 4 3 2)"
  },
  {
    "id": "tail-calls",
    "section": "Functions",
    "title": "尾调用",
    "text": "尾调用优化（TCO）保证递归不会爆栈：如果递归调用是函数的最后一个动作，VM 会复用当前栈帧。这让深度递归和无限循环成为可能。这里 sum 递归一百万次依然安全地返回。",
    "code": "fn sum(r, i) {\n  if i == 0 {\n    r\n  } else {\n    sum(r + 1, i - 1)\n  }\n}\n\nfn main() {\n  print(sum(0, 1000000))\n}",
    "expected": "1000000"
  },
  {
    "id": "if-else",
    "section": "Flow control",
    "title": "if / else",
    "text": "if 是表达式而不是语句：每个分支都返回一个值，整个 if 表达式的值就是所选分支的值。没有 else 时，假分支的值是 nil。多个条件可以用 else if 连成一条链（if a { } else if b { } else { }），等价于嵌套 if 但更清晰。",
    "code": "fn classify(n) {\n  if n > 0 {\n    \"positive\"\n  } else if n == 0 {\n    \"zero\"\n  } else {\n    \"negative\"\n  }\n}\n\nfn main() {\n  print(classify(5))\n  print(classify(0))\n  print(classify(-3))\n}",
    "expected": "positive\nzero\nnegative"
  },
  {
    "id": "match-basics",
    "section": "Flow control",
    "title": "match 基础",
        "text": "match 是表达式：按顺序尝试每个分支，第一个匹配上的分支的值就是整个 match 的结果。构造子模式同时做两件事——判断加拆包：Cat(name, age) 要求这个值是 Cat 构造出来的，并把两个字段交给 name、age，不用的字段写 _。这里的 Animal 先用着看效果，type 声明本身在第 16 课细讲；注意 describe 是真的在读字段，名字和年龄都进了结果。",
    "code": "type Animal {\n  Cat(String, Int)\n  Dog(String, Int)\n}\n\nfn describe(a) {\n  match a {\n    Cat(name, age) -> str.concat(str.concat(name, \" is a cat aged \"), str.from_int(age))\n    Dog(name, _) -> str.concat(name, \" is a dog\")\n  }\n}\n\nfn main() {\n  print(describe(Cat(\"Nubi\", 2)))\n  print(describe(Dog(\"Rex\", 3)))\n}",
    "expected": "Nubi is a cat aged 2\nRex is a dog"
  },
  {
    "id": "pattern-syntax",
    "section": "Flow control",
    "title": "模式语法",
        "text": "模式可以组合着用：字面量能嵌在构造子里（Cat(name, 1) 只命中一岁的猫，后面的 Cat(name, age) 接住其它年龄）；[x | rest] 把列表拆成头和尾（[1, 2, 3] 拆成 1 和 [2, 3]），[x, y] 钉死长度，[x | _] 表示只要头、尾巴扔掉；when 守卫能引用模式绑定的变量，守卫为假就接着往下试。分支自上而下尝试，所以具体的写前面、兜底的写后面。",
    "code": "type Animal {\n  Cat(String, Int)\n  Dog(String, Int)\n}\n\nfn sum(lst) {\n  match lst {\n    [] -> 0\n    [x | rest] -> x + sum(rest)\n  }\n}\n\nfn describe(a) {\n  match a {\n    Cat(name, 1) -> str.concat(name, \" is one year old\")\n    Cat(name, age) -> str.concat(str.concat(name, \" is aged \"), str.from_int(age))\n    Dog(name, _) -> str.concat(name, \" is a dog\")\n  }\n}\n\nfn classify(n) {\n  match n {\n    0 -> \"zero\"\n    n when n < 0 -> \"negative\"\n    _ -> \"positive\"\n  }\n}\n\nfn head_or(lst, fallback) {\n  match lst {\n    [x | _] -> x\n    [] -> fallback\n  }\n}\n\nfn main() {\n  print(sum([1, 2, 3, 4]))\n  print(describe(Cat(\"Nubi\", 1)))\n  print(describe(Cat(\"Kiwi\", 5)))\n  print(classify(-5))\n  match [3, 4] {\n    [x, y] -> print(x + y)\n    _ -> print(0)\n  }\n  print(head_or([7, 8, 9], 0))\n  print(head_or([], -1))\n}",
    "expected": "10\nNubi is one year old\nKiwi is aged 5\nnegative\n7\n7\n-1"
  },
  {
    "id": "exhaustiveness",
    "section": "Flow control",
    "title": "穷尽性检查",
    "text": "编译器会对 ADT 的 match 做穷尽性检查：漏掉某个变体又没有 _ 兜底，就是硬错误（[E0005] non-exhaustive match: missing Green, Blue），编译直接中止，不会放你过去。穷尽匹配让编译器帮你确认没有漏掉任何分支。这里 match 覆盖了 Color 的全部三个变体，所以能编译通过。",
    "code": "type Color { Red; Green; Blue }\n\nfn color_name(c) {\n  match c {\n    Red -> print(\"red\")\n    Green -> print(\"green\")\n    Blue -> print(\"blue\")\n  }\n}\n\nfn main() {\n  color_name(Red)\n  color_name(Green)\n  color_name(Blue)\n  print(\"PASS\")\n}",
    "expected": "red\ngreen\nblue\nPASS"
  },
  {
    "id": "lists",
    "section": "Data types",
    "title": "列表",
    "text": "列表是 nil 结尾的 cons 链：[a, b, c] 等价于 cons(a, cons(b, cons(c, nil)))。car 取头、cdr 取尾、null? 判空。元素必须同型——[1, \"a\"] 编译期就报错。cons 只用来拼列表（cdr 必须是 nil 或另一个 cons）；点对、记录这种定长数据用 ADT 表达，不要写 cons(1, 2)。",
    "code": "fn len(l) {\n  if null?(l) {\n    0\n  } else {\n    1 + len(cdr(l))\n  }\n}\n\nfn main() {\n  let lst = [1, 2, 3]\n  print(lst)           // (1 2 3)\n  print(car(lst))      // 1\n  print(cdr(lst))      // (2 3)\n  print(len(lst))      // 3\n  print(cons(0, lst))  // (0 1 2 3)\n}",
    "expected": "(1 2 3)\n1\n(2 3)\n3\n(0 1 2 3)"
  },
  {
    "id": "adt",
    "section": "Data types",
    "title": "ADT 代数数据类型",
    "text": "type 声明代数数据类型：一个类型可以有多个构造器，每个构造器带不同的参数。构造器名必须严格匹配，用不同构造器或不同参数个数构造的值不会误匹配。ADT 让数据形状自文档化。带参构造子在运行时就是一个带标签的元组：Point(1, 2) 是 (tuple Point 1 2)，tuple-len / tuple-ref 能读它（日常用模式解构，不必碰底层表示）；点对、记录都该这么表达，而不是 cons。",
    "code": "type Msg {\n  Add(a, b)\n  Mul(a, b)\n}\n\nfn eval(m) {\n  match m {\n    Add(a, b) -> a + b\n    Mul(a, b) -> a * b\n  }\n}\n\nfn main() {\n  print(eval(Add(2, 5)))\n  print(eval(Mul(2, 5)))\n}",
    "expected": "7\n10"
  },
  {
    "id": "generic-adt",
    "section": "Data types",
    "title": "泛型 ADT",
    "text": "ADT 可以带类型参数，像 List(a) 这样对任意元素类型复用。Nil 是空列表，Cons(head, tail) 把一个元素接到列表前面。类型推导能推出这是 List(Int)：Cons(1, Cons(2, Cons(3, Nil)))。",
    "code": "type List(a) { Nil; Cons(a, List(a)) }\n\nfn sum(lst) {\n  match lst {\n    Nil -> 0\n    Cons(head, tail) -> head + sum(tail)\n  }\n}\n\nfn main() {\n  let lst = Cons(1, Cons(2, Cons(3, Nil)))\n  print(sum(lst))\n}",
    "expected": "6"
  },
  {
    "id": "result",
    "section": "Data types",
    "title": "Result 处理错误",
    "text": "Result(a, e) 是处理错误的惯用 ADT：Ok(v) 表示成功，Error(e) 表示失败（标准库 lib/result.ta 里有形状相同的 Result(t, e)，但构造器叫 Ok / Err；这里为了不用 import 先自己声明一份）。函数返回 Result，调用方用 match 显式处理两种情况。这避免了异常，让错误成为数据流的一部分。",
    "code": "type Result(a, e) { Ok(a); Error(e) }\n\nfn safe_div(a, b) {\n  if b == 0 {\n    Error(\"division by zero\")\n  } else {\n    Ok(a / b)\n  }\n}\n\nfn main() {\n  match safe_div(10, 2) {\n    Ok(v) -> print(v)\n    Error(e) -> print(e)\n  }\n  match safe_div(1, 0) {\n    Ok(v) -> print(v)\n    Error(e) -> print(e)\n  }\n}",
        "expected": "5\ndivision by zero"
  },
  {
    "id": "type-annotations",
    "section": "Type system",
    "title": "类型签名",
    "text": "fn fib(n: Int) -> Int 是类型签名：参数和返回值都标注类型（类型名首字母大写）。注解是可选的——不写也能编译；写了之后，类型检查器会在编译期验证函数体是否符合声明，不匹配直接报错。坑在这里：小写 int 不是整数类型，裸小写名是隐式类型参数，写 fn f(n: int) -> int 等于说「n 和返回值都没定」，检查器会拿函数体去反推它（而且报错会指向调用点，不是注解）。",
    "code": "fn fib(n: Int) -> Int {\n  if n < 2 {\n    n\n  } else {\n    fib(n - 1) + fib(n - 2)\n  }\n}\n\nfn main() {\n  print(fib(20))\n}",
    "expected": "6765"
  },
  {
    "id": "type-inference",
    "section": "Type system",
    "title": "类型推导",
    "text": "注解不是必须的：编译器用 Hindley-Milner 算法从使用方式推导类型。inc 里 n + 1 推出 n 是整数；twice 里 f 被应用了两次，推出 f 接收和返回同一类型。全程没有一个注解，程序照样类型安全。",
    "code": "fn twice(f, x) {\n  f(f(x))\n}\n\nfn inc(n) {\n  n + 1\n}\n\nfn main() {\n  print(twice(inc, 40))\n}",
    "expected": "42"
  },
  {
    "id": "spawn",
    "section": "Actors",
    "title": "spawn 启动进程",
    "text": "spawn(fn { ... }) 启动一个新的 actor 进程并返回它的 pid。新进程有自己的栈和邮箱，与主进程并发运行。self() 返回当前进程的 pid。actor 之间只能通过消息通信，没有共享内存。",
    "code": "fn worker(main_pid) {\n  send(main_pid, 'hello)\n}\n\nfn main() {\n  let me = self()\n  let pid = spawn(fn { worker(me) })\n  match recv() {\n    'hello -> print(\"got message\")\n  }\n}",
    "expected": "got message"
  },
  {
    "id": "send-recv",
    "section": "Actors",
    "title": "send / recv 消息",
    "text": "send(pid, msg) 异步发送消息，recv() 阻塞接收下一条消息，邮箱是 FIFO 队列。消息可以是任何普通值：整数、字符串、ADT 值（元组），甚至 pid 本身（C 模块的 Resource 例外，它按归属移交给接收方，有额外的传递限制）。这里进程给自己发一条消息再收回来。",
    "code": "fn main() {\n  send(self(), 42)\n  print(recv())\n}",
    "expected": "42"
  },
  {
    "id": "selective-receive",
    "section": "Actors",
    "title": "选择性接收",
    "text": "receive { pattern -> body } 会扫描邮箱，跳过不匹配的消息，直到找到匹配的。这突破了严格 FIFO 的限制：即使 'first 先到，也可以先接收 'second。被跳过的消息留在邮箱里，之后还能收到。",
    "code": "fn server() {\n  receive {\n    'second -> print(\"got-second\")\n  }\n  receive {\n    'first -> print(\"then-first\")\n  }\n  receive {\n    n -> if n == 42 { print(\"PASS\") } else { print(\"FAIL\") }\n  }\n}\n\nfn main() {\n  let srv = spawn(fn { server() })\n  send(srv, 'first)\n  send(srv, 'second)\n  send(srv, 42)\n  let ref = monitor(srv)\n  receive {\n    ['DOWN, r, pid, reason] -> print(\"server-done\")\n  }\n}",
    "expected": "got-second\nthen-first\nPASS\nserver-done"
  },
  {
    "id": "monitor-down",
    "section": "Actors",
    "title": "monitor / DOWN",
    "text": "monitor(pid) 监控另一个进程并返回一个 ref。被监控进程死亡时，监控方会收到 ['DOWN, ref, pid, reason] 消息。这是构建 supervisor 的基础。这里 worker 除零崩溃，main 收到 DOWN 通知；VM 还会额外打一行 ** CRASH 报告（带 pid 与帧号），那不是 print 的输出。",
    "code": "fn worker() {\n  1 / 0\n}\n\nfn main() {\n  let pid = spawn('worker)\n  let ref = monitor(pid)\n  match recv() {\n    ['DOWN, r, p, reason] -> print(\"DOWN received\")\n  }\n}",
    "expected": "DOWN received"
  },
  {
    "id": "process-isolation",
    "section": "Actors",
    "title": "进程隔离",
    "text": "actor 崩溃只影响自己，不影响其他进程。crasher 除零崩溃，但 main 进程照常运行并打印 survived。配合 monitor，一个进程的错误可以被另一个进程检测和恢复。这体现了 actor 模型的容错哲学。控制台里的 ** CRASH 报告是 VM 打的，不是 print 的输出。",
    "code": "fn crasher() {\n  1 / 0\n}\n\nfn main() {\n  spawn('crasher)\n  send(self(), 'hello)\n  recv()\n  print(\"survived\")\n}",
    "expected": "survived"
  },
  {
    "id": "modules-import-pub",
    "section": "Modules",
    "title": "import / pub",
        "text": "pub fn 标记可导出：本文件里直接调用，别的模块 import 之后才能调用。import 后面是带模块树名的路径字符串——import \"tinyactor:lib/list\"，不是裸名字 import list；引入后按「模块名.成员」调用，如 list.length、list.reverse。C 后端模块（str、os、file）的成员是内建的，不用 import 就能用。",
    "code": "import \"tinyactor:lib/list\"\n\npub fn double(x) {\n  x * 2\n}\n\nfn main() {\n  print(double(21))\n  print(list.length([1, 2, 3]))\n  print(list.reverse([1, 2, 3]))\n}",
    "expected": "42\n3\n(3 2 1)"
  }
];