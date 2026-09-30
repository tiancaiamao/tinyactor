#!/usr/bin/env python3
# expand_imports.py — 把 TA 程序的 import 树文本级展开成单文件，供 lisp 管线
# （单命名空间）吃下。与宿主编译器的模块语义对齐：
#   - import 在编译期并入（每模块一个编译单元，链接期 ns 隔离）
#   - 跨模块同名顶层 fn 在 lisp 单 ns 里会 duplicate global —— 因此对冲突
#     名做 per-module 重命名：名字首次定义的模块保留原名，后续模块的
#     定义与裸名引用统一加后缀 ___<mod>。dotted 引用（mod.fn）不受影响，
#     那是运行期 cfunc（str/file/buf…）或链接期解析的另一个世界。
# 用法： python3 expand_imports.py lib/bootstrap/driver.ta vm-demo/lisp/boot/driver-all.ta
import re
import sys

IMPORT_RE = re.compile(r'^\s*import\s+([A-Za-z_][A-Za-z0-9_]*)\s*$', re.M)


def module_candidates(name, importer_dir):
    return [
        importer_dir + "/" + name + ".ta",
        importer_dir + "/helpers/" + name + ".ta",
        "lib/" + name + ".ta",
        "lib/bootstrap/" + name + ".ta",
    ]


seen = {}    # path -> module short name
texts = {}   # path -> stripped text
order = []   # load order

FN_DEF_RE = re.compile(r'^(?:pub\s+)?fn\s+([a-z_][a-z_0-9]*)', re.M)


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
for path in order:
    for name in FN_DEF_RE.findall(texts[path]):
        if name in owner:
            if owner[name] != path:
                suffix[(path, name)] = name + "___" + seen[path]
        else:
            owner[name] = path

out = []
for path in order:
    text = texts[path]
    ren = {n: suffix[(path, n)] for (p, n) in suffix if p == path}
    if ren:
        # 裸名替换：词边界，排除 dotted 引用（.name）与更长标识符
        for name, new in ren.items():
            text = re.sub(r'(?<![.\w])' + name + r'(?![\w])', new, text)
    # dotted 引用解析：mod.fn → 模块内 fn 的最终名（TA 模块系统在编译期
    # 做同样的名字解析；留着 dotted 会被 lispvm 当运行期 cfunc 去找不存在的
    # dylib）。只替换已展开模块的名字，str.concat 这类 cfunc dotted 不动。
    mod = seen[path]
    for target_mod in order:
        tmod = seen[target_mod]
        if tmod == mod:
            continue
        exports = set(FN_DEF_RE.findall(texts[target_mod]))
        exports |= {suffix[(target_mod, n)] for (p, n) in suffix if p == target_mod}

        def sub(m):
            fn = m.group(1)
            if fn in exports:
                return fn
            return m.group(0)
        text = re.sub(r'(?<![.\w])' + tmod + r'\.([a-z_][a-z_0-9]*)', sub, text)
    out.append(text)

result = "\n".join(out)
with open(sys.argv[2], "w") as f:
    f.write(result)
renamed = sum(1 for v in suffix.values())
print(f"expanded {len(order)} files -> {sys.argv[2]} "
      f"({len(result.splitlines())} lines, {renamed} fns renamed)")