#!/usr/bin/env python3
"""Summarize TA coverage dumps against a TinyActor covmap.

Covmap rows: "k file:line name" — line 0 marks a function entry (the
gate's unit), line > 0 a statement / match-arm body.
"""

import sys
from pathlib import Path


def parse_covmap(covmap):
    rows = {}
    for line_number, line in enumerate(covmap.read_text().splitlines(), 1):
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            raise ValueError(f"invalid coverage map row {covmap}:{line_number}")
        ident = int(fields[0])
        if ident in rows:
            raise ValueError(f"duplicate coverage id {ident} in {covmap}:{line_number}")
        loc, name = fields[1], fields[2]
        file_name, sep, line_str = loc.rpartition(":")
        if not sep or not file_name or not line_str.isdigit():
            raise ValueError(f"invalid location {loc!r} in {covmap}:{line_number}")
        rows[ident] = (file_name, int(line_str), name)
    return rows


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: coverage_ta.py <covmap> <dump-dir> <report>")
    covmap, dump_dir, report_path = map(Path, sys.argv[1:])
    rows = parse_covmap(covmap)

    counts = {ident: 0 for ident in rows}
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

    def pct(hit, total):
        return 100 * hit / total if total else 0

    fn_ids = [i for i in rows if rows[i][1] == 0]
    stmt_ids = [i for i in rows if rows[i][1] > 0]
    fn_hit = sum(counts[i] > 0 for i in fn_ids)
    stmt_hit = sum(counts[i] > 0 for i in stmt_ids)
    lines = [
        f"TA function coverage: {fn_hit}/{len(fn_ids)} ({pct(fn_hit, len(fn_ids)):.2f}%)",
        f"TA statement coverage: {stmt_hit}/{len(stmt_ids)} ({pct(stmt_hit, len(stmt_ids)):.2f}%)",
        "",
    ]
    order = sorted(rows, key=lambda i: (rows[i][0], rows[i][1], rows[i][2], i))
    lines.extend(
        f"{'HIT ' if counts[i] else 'MISS'} {counts[i]:>8}  {rows[i][0]}:{rows[i][1]} {rows[i][2]}"
        for i in order
    )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n")
    print(lines[0])
    print(lines[1])
    print(f"report: {report_path}")


if __name__ == "__main__":
    main()