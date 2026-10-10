#!/usr/bin/env python3
# expand_imports.py — 把 TA 程序的 import 树文本级展开成单文件，供 lisp 管线
# （单命名空间）吃下。与宿主编译器的模块语义对齐：
#   - import 在编译期并入（每模块一个编译单元，链接期 ns 隔离）
#   - 跨模块同名顶层 fn 在 lisp 单 ns 里会 duplicate global —— 因此对冲突
#     名做 per-module 重命名：名字首次定义的模块保留原名，后续模块的
#     定义与裸名引用统一加后缀 ___<mod>。dotted 引用（mod.fn）不受影响，
#     那是运行期 cfunc（str/file/buf…）或链接期解析的另一个世界。
# 用法： python3 expand_imports.py lib/bootstrap/driver.ta /tmp/driver-all.ta
import re
import sys

IMPORT_RE = re.compile(r'^\s*import\s+([A-Za-z_][A-Za-z0-9_-]*)\s*$', re.M)

# 项目根 = 本脚本上两级（<root>/tools/expand_imports.py）。lib 候选
# 必须相对项目根解析：CLI 从外部 CWD 调 tinyactor run 时，进程 CWD 不是
# 项目根，相对 "lib/x.ta" 会静默 miss → import 行被删 → 点式调用运行期
# 按 cfunc 找不到 → 静默 nil（CLI 测试的 fs.mkdir_p 踩过）。
_PROJECT_ROOT = __file__.rsplit("/", 2)[0] if "/" in __file__ else "."


def module_candidates(name, importer_dir):
    return [
        importer_dir + "/" + name + ".ta",
        importer_dir + "/helpers/" + name + ".ta",
        _PROJECT_ROOT + "/lib/" + name + ".ta",
        _PROJECT_ROOT + "/lib/bootstrap/" + name + ".ta",
    ]


seen = {}    # path -> module short name
texts = {}   # path -> stripped text
order = []   # load order

FN_DEF_RE = re.compile(r'^(?:pub\s+)?fn\s+([a-z_][a-z_0-9]*)', re.M)
CONST_DEF_RE = re.compile(r'^(?:pub\s+)?const\s+([A-Za-z_][A-Za-z0-9_]*)', re.M)

# 模块名允许连字符（lower-ast），但 ___<mod> 改名后缀必须仍是合法标识符。
def mod_tag(mod):
    return mod.replace("-", "_")


def load(path):
    if path in seen:
        return
    mod = path.rsplit("/", 1)[-1][:-3]
    seen[path] = mod
    text = open(path).read()
    dirname = path.rsplit("/", 1)[0] if "/" in path else "."
    # TA 源码模块由本脚本展开；C dylib 模块（str/file/buf… 无 .ta 源码）
    # 的 import 行删掉即可——其 dotted 调用运行期走 cfunc dlopen。
    texts[path] = IMPORT_RE.sub("", text)
    order.append(path)
    for name in IMPORT_RE.findall(text):
        for c in module_candidates(name, dirname):
            try:
                load(c)
                break
            except OSError:
                continue


load(sys.argv[1])

# 冲突名 -> 已占用。按 load 顺序，首个定义者保留，后续模块重命名。
owner = {}
suffix = {}  # (path, name) -> new name
# and/or 是 TA 的短路基（||/&& 在 AST 里就是 ('or ...)/('and ...) 头，
# 宿主 codegen/typecheck 内建）。模块里定义的 fn or / fn and 经展开去掉
# 模块前缀后与算子头不可区分，lower-ast 的 and/or 降级会劫持真调用
# （option.or 的 is_some 语义被换成裸真值判断 → unwrap 崩）。
# 一律改名成 or___<mod>，裸名/点名校由下面的通用改名机制兜住。
RESERVED_OPS = ("and", "or")
for path in order:
    for name in FN_DEF_RE.findall(texts[path]):
        if name in RESERVED_OPS:
            suffix[(path, name)] = name + "___" + mod_tag(seen[path])
        elif name in owner:
            if owner[name] != path:
                suffix[(path, name)] = name + "___" + mod_tag(seen[path])
        else:
            owner[name] = path

def sub_outside_strings(pattern, repl, text):
    # 字符串字面量里的 "os.args"、注释里的示例、字符/符号字面量都不是引用。
    # 小型扫描器四态：// 注释、".." 字符串、'x' 字符字面量、'ident 符号
    # 字面量——只有代码区做替换。TA 无块注释、无多行字符串。
    # 四态扫描器的 IDENT 跟随导入名放宽到连字符：'type-sig 这类符号字面量
    # 要整个吞掉，否则按 't 字符字面量消费、"ype-sig" 落回代码区错位。
    IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_-]*')
    res = []

    def emit(a, b):
        if b > a:
            res.append(re.sub(pattern, repl, text[a:b]))

    i, n, last = 0, len(text), 0
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j == -1 else j
            emit(last, i)
            res.append(text[i:j])
            last = i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            emit(last, i)
            res.append(text[i:j])
            last = i = j
        elif c == "'":
            m = IDENT.match(text, i + 1)
                        # 符号字面量 'ok：吞引号+标识符。'e' 这类单字符字面量也命中
            # IDENT——若后随闭引号必须按字符字面量扫，否则留下孤儿闭引号
            # 使引号配对整体错位，改名/点名解析在错位段静默失效。
            if m and text[m.end():m.end() + 1] != "'":
                j = m.end()
            else:  # 字符字面量 'x' / '\n'
                j = i + 1
                while j < n and text[j] != "'":
                    j += 2 if text[j] == '\\' else 1
                j = min(j + 1, n)
            emit(last, i)
            res.append(text[i:j])
            last = i = j
        else:
            i += 1
    emit(last, n)
    return "".join(res)

out = []
for path in order:
    text = texts[path]
    ren = {n: suffix[(path, n)] for (p, n) in suffix if p == path}
    if ren:
        # 裸名替换：词边界，排除 dotted 引用（.name）与更长标识符
        for name, new in ren.items():
            text = sub_outside_strings(r'(?<![.\w])' + name + r'(?![\w])', new, text)
    # dotted 引用解析：mod.fn → 模块内 fn 的最终名（TA 模块系统在编译期
    # 做同样的名字解析；留着 dotted 会被 tavm 当运行期 cfunc 去找不存在的
    # dylib）。只替换已展开模块的名字，str.concat 这类 cfunc dotted 不动。
    mod = seen[path]
    for target_mod in order:
        tmod = seen[target_mod]
        if tmod == mod:
            continue
        exports = set(FN_DEF_RE.findall(texts[target_mod]))
        exports |= {suffix[(target_mod, n)] for (p, n) in suffix if p == target_mod}
        # 大写 const（log.ERROR）：内核 parser 的 const 表只认裸名，点名校
        # 必须在这里解掉，否则运行期按名 miss 成 nil（log-lib 阈值测试）
        exports |= set(CONST_DEF_RE.findall(texts[target_mod]))

        def sub(m):
            fn = m.group(1)
            # and/or 一律走改名表：exports 里的原始名只是占位，不能命中
            if (target_mod, fn) in suffix:
                return suffix[(target_mod, fn)]
            if fn in exports:
                return fn
            return m.group(0)
        text = sub_outside_strings(r'(?<![.\w])' + tmod + r'\.([A-Za-z_][A-Za-z0-9_]*)', sub, text)
    out.append(text)

result = "\n".join(out)
with open(sys.argv[2], "w") as f:
    f.write(result)
renamed = sum(1 for v in suffix.values())
print(f"expanded {len(order)} files -> {sys.argv[2]} "
      f"({len(result.splitlines())} lines, {renamed} fns renamed)")