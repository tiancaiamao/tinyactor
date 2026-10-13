#!/usr/bin/env python3
"""check_module_imports.py — module-gate half B: direct-declaration scan.

Every dotted module reference `M.f(...)` in a lib/*.ta / lib/bootstrap/*.ta
unit must be covered by that unit's OWN declarations:
  * `import M` in the same file, or
  * `external fn M.g(...)` in the same file (lazy-dylib C signature layer,
    e.g. fs.ta's file.stat), or
  * a compiler-promised builtin (typecheck.ta seeds precise types/arity):
      - str low-level primitives (lib/str.ta header documents the split:
        these are deliberately available WITHOUT `import str`),
      - file.read / file.write / file.exists, int.to_float,
      - host C modules vm / net (is_builtin_module answers yes for them).
  * the unit itself (bare self-qualified references).

Everything else is a free ride on the flat closure (some sibling unit's
import registered the export globally) and is reported as a violation.
Comments and string literals are stripped before scanning.
"""
import glob
import os
import re
import sys

PROMISED_STR = {
    "char_at", "chr", "length", "substr", "concat", "to_int", "from_int",
    "eq", "index_of", "to_sym", "sym_to_str",
}
PROMISED_FNS = {  # mod -> set of promised fn names (whole-mod otherwise)
    "file": {"read", "write", "exists"},
    "int": {"to_float"},
}
PROMISED_MODS = {"vm", "net"}  # host C modules: always importable

HEAD = re.compile(r"\b([a-z_][a-z0-9_-]*)\.([a-z_]+)\s*\(")
IMPORT = re.compile(r"^import\s+([a-z_][a-z0-9_-]*)", re.M)
EXTERNAL = re.compile(r"^external fn ([a-z_][a-z0-9_-]*)\.([a-z_]+)", re.M)


def strip_noise(text):
    # remove line comments, then string literals (their contents often
    # quote call syntax in error messages)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return text


def main():
    violations = []
    files = sorted(glob.glob("lib/*.ta")) + sorted(glob.glob("lib/bootstrap/*.ta"))
    for path in files:
        raw = open(path).read()
        text = strip_noise(raw)
        self_name = os.path.splitext(os.path.basename(path))[0]
        declared = set(IMPORT.findall(text))
        ext_mods = {m for m, _ in EXTERNAL.findall(text)}
        for mod, fn in set(HEAD.findall(text)):
            if mod == self_name or mod in declared or mod in ext_mods:
                continue
            if mod in PROMISED_MODS:
                continue
            if mod == "str" and fn in PROMISED_STR:
                continue
            if mod in PROMISED_FNS and fn in PROMISED_FNS[mod]:
                continue
            violations.append(f"{path}: {mod}.{fn} — no import/external fn in this unit")
    if violations:
        print("\n".join(sorted(violations)))
    sys.exit(1 if violations else 0)


if __name__ == "__main__":
    main()