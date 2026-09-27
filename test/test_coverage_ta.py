#!/usr/bin/env python3
"""Tests for the TA coverage report aggregator."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    script = Path(__file__).resolve().parents[1] / "tools/coverage_ta.py"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        covmap = root / "image.covmap"
        covmap.write_text("0 lib/a.ta:0 alpha\n1 lib/a.ta:5 alpha\n2 lib/b.ta:0 gamma\n3 lib/b.ta:7 gamma\n4 lib/c.ta:0 beta\n")
        dumps = root / "dumps"
        dumps.mkdir()
        (dumps / "one.cov").write_text("0 2\n1 0\n")
        (dumps / "two.cov").write_text("0 3\n2 4\n3 1\n")
        report = root / "report.txt"
        subprocess.run(
            [sys.executable, str(script), str(covmap), str(dumps), str(report)],
            check=True,
            capture_output=True,
            text=True,
        )
        output = report.read_text()
        assert "TA function coverage: 2/3 (66.67%)" in output
        assert "TA statement coverage: 1/2 (50.00%)" in output
        assert "HIT" in output and "5  lib/a.ta:0 alpha" in output
        assert "MISS" in output and "0  lib/a.ta:5 alpha" in output
        assert "HIT" in output and "4  lib/b.ta:0 gamma" in output

        (dumps / "bad.cov").write_text("7 1\n")
        result = subprocess.run(
            [sys.executable, str(script), str(covmap), str(dumps), str(report)],
            capture_output=True,
            text=True,
        )
        assert result.returncode != 0

        (dumps / "bad.cov").unlink()
        (root / "bad.covmap").write_text("0 noline alpha\n")
        result = subprocess.run(
            [sys.executable, str(script), str(root / "bad.covmap"), str(dumps), str(report)],
            capture_output=True,
            text=True,
        )
        assert result.returncode != 0
        print("coverage report tests: 9 assertions passed")


if __name__ == "__main__":
    main()