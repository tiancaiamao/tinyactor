#!/usr/bin/env python3
"""Merge TA coverage maps without conflating per-image coverage IDs."""

import sys
from pathlib import Path


def parse_map(path):
    rows = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            raise ValueError(f"invalid coverage map row {path}:{number}")
        ident = int(fields[0])
        loc, name = fields[1], fields[2]
        filename, sep, line_text = loc.rpartition(":")
        if not sep or not line_text.isdigit():
            raise ValueError(f"invalid coverage location {loc!r} in {path}:{number}")
        rows[ident] = (filename, int(line_text), name)
    return rows


def main():
    if len(sys.argv) != 6:
        raise SystemExit("usage: merge_coverage_ta.py <bootstrap-map> <bootstrap-dumps> <programs-dir> <merged-map> <merged-dumps>")
    bootstrap_map, bootstrap_dumps, programs, out_map, out_dumps = map(Path, sys.argv[1:])
    groups = [(bootstrap_map, bootstrap_dumps)]
    for map_path in programs.glob("maps/*/map.covmap"):
        groups.append((map_path, programs / "dumps" / map_path.parent.name))

    hits = {}
    for map_path, dump_dir in groups:
        rows = parse_map(map_path)
        counts = {ident: 0 for ident in rows}
        if dump_dir.exists():
            for dump in dump_dir.iterdir():
                if not dump.is_file():
                    continue
                for line in dump.read_text().splitlines():
                    ident_text, count_text = line.split()
                    ident, count = int(ident_text), int(count_text)
                    if ident not in counts or count < 0:
                        raise ValueError(f"invalid coverage entry {ident} in {dump}")
                    counts[ident] += count
        for ident, location in rows.items():
            hits[location] = hits.get(location, 0) + counts[ident]

    out_map.parent.mkdir(parents=True, exist_ok=True)
    out_dumps.mkdir(parents=True, exist_ok=True)
    map_lines, dump_lines = [], []
    for ident, (location, count) in enumerate(sorted(hits.items())):
        filename, line, name = location
        map_lines.append(f"{ident} {filename}:{line} {name}")
        dump_lines.append(f"{ident} {count}")
    out_map.write_text("\n".join(map_lines) + "\n")
    (out_dumps / "merged.cov").write_text("\n".join(dump_lines) + "\n")


if __name__ == "__main__":
    main()
