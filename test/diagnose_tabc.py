#!/usr/bin/env python3
"""Print a compact structural diff for two TABC files."""

import sys
from collections import Counter


def parse(path):
    data = open(path, "rb").read()

    def u32(offset):
        if offset + 4 > len(data):
            raise ValueError(f"truncated u32 at offset {offset}")
        return int.from_bytes(data[offset : offset + 4], "little")

    if len(data) < 24 or data[:4] != b"TABC":
        raise ValueError("not a TABC file")

    header = {
        "size": len(data),
        "version": u32(4),
        "n_symbols": u32(8),
        "n_fns": u32(12),
        "top_fn": u32(16),
        "code_len": u32(20),
    }
    symbols = []
    pos = 24
    for _ in range(header["n_symbols"]):
        length = u32(pos)
        pos += 4
        end = pos + length
        if end > len(data):
            raise ValueError(f"truncated symbol at offset {pos}")
        symbols.append(data[pos:end].decode("utf-8", errors="replace"))
        pos = end
    header["symbols_end"] = pos
    return data, header, symbols


def limited(values, count=20):
    values = list(values)
    suffix = "" if len(values) <= count else f" ... ({len(values)} total)"
    return repr(values[:count]) + suffix


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} REFERENCE REBUILT")

    parsed = []
    for path in sys.argv[1:]:
        try:
            parsed.append(parse(path))
        except (OSError, ValueError) as error:
            raise SystemExit(f"{path}: {error}")

    (a, ah, asyms), (b, bh, bsyms) = parsed
    limit = min(len(a), len(b))
    first_byte = next((i for i in range(limit) if a[i] != b[i]), limit)
    diff_count = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
    print(f"TABC diagnostic: differing bytes {diff_count}, first offset {first_byte} (0x{first_byte:x})")
    print(f"reference header: {ah}")
    print(f"rebuilt header:   {bh}")

    first_sym = next(
        (i for i, (left, right) in enumerate(zip(asyms, bsyms)) if left != right),
        min(len(asyms), len(bsyms)),
    )
    if first_sym < max(len(asyms), len(bsyms)):
        left = asyms[first_sym] if first_sym < len(asyms) else "<missing>"
        right = bsyms[first_sym] if first_sym < len(bsyms) else "<missing>"
        print(f"first symbol difference: index {first_sym}: {left!r} vs {right!r}")

    acounts = Counter(asyms)
    bcounts = Counter(bsyms)
    only_a = sorted((name, count - bcounts[name]) for name, count in acounts.items() if count > bcounts[name])
    only_b = sorted((name, count - acounts[name]) for name, count in bcounts.items() if count > acounts[name])
    dup_a = sorted((name, count) for name, count in acounts.items() if count > 1)
    dup_b = sorted((name, count) for name, count in bcounts.items() if count > 1)
    print(f"symbols only/excess in reference: {limited(only_a)}")
    print(f"symbols only/excess in rebuilt:   {limited(only_b)}")
    print(f"duplicate symbols in reference:  {limited(dup_a)}")
    print(f"duplicate symbols in rebuilt:    {limited(dup_b)}")


if __name__ == "__main__":
    main()