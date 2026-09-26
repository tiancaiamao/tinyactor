#!/usr/bin/env python3
"""Summarize function-entry coverage dumps against a TinyActor covmap."""

import sys
from pathlib import Path


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: coverage_ta.py <covmap> <dump-dir> <report>")
    covmap, dump_dir, report_path = map(Path, sys.argv[1:])
    names = {}
    for line_number, line in enumerate(covmap.read_text().splitlines(), 1):
        fields = line.split(maxsplit=1)
        if len(fields) != 2:
            raise ValueError(f"invalid coverage map row {covmap}:{line_number}")
        ident = int(fields[0])
        if ident in names:
            raise ValueError(f"duplicate coverage id {ident} in {covmap}:{line_number}")
        names[ident] = fields[1]

    counts = {ident: 0 for ident in names}
    for dump in dump_dir.iterdir():
        if not dump.is_file():
            continue
        for line_number, line in enumerate(dump.read_text().splitlines(), 1):
            fields = line.split()
            if len(fields) != 2:
                raise ValueError(f"invalid coverage dump row {dump}:{line_number}")
            ident, count = map(int, fields)
            if ident not in counts or count < 0:
                raise ValueError(f"invalid coverage entry {ident} in {dump}:{line_number}")
            counts[ident] += count

    hit = sum(count > 0 for count in counts.values())
    total = len(names)
    lines = [f"TA function coverage: {hit}/{total} ({100 * hit / total if total else 0:.2f}%)", ""]
    lines.extend(f"{'HIT ' if counts[ident] else 'MISS'} {counts[ident]:>8}  {names[ident]}" for ident in sorted(names))
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n")
    print(lines[0])
    print(f"report: {report_path}")


if __name__ == "__main__":
    main()
