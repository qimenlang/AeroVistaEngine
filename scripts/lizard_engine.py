#!/usr/bin/env python3
"""Run lizard cyclomatic-complexity (CCN) checks on engine sources.

Used by pre-commit and CI. No compile_commands.json required.
Install: pip install lizard

Examples:
  python scripts/lizard_engine.py --all-source
  python scripts/lizard_engine.py engine/source/function/sync/IgSync.cpp
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Match cognitive-complexity scope: production code only (skip Catch2 Tests).
DEFAULT_ROOT = ROOT / "engine" / "source"


def collect_files(paths: list[str], all_source: bool) -> list[Path]:
    if all_source or not paths:
        return sorted(DEFAULT_ROOT.rglob("*.cpp"))

    tus: list[Path] = []
    for path in paths:
        resolved = Path(path) if Path(path).is_absolute() else (ROOT / path)
        try:
            resolved = resolved.resolve()
        except OSError:
            continue
        if not resolved.is_file():
            continue
        text = str(resolved).replace("\\", "/")
        if "/engine/source/" not in text:
            continue
        if text.lower().endswith((".cpp", ".cc", ".cxx")):
            tus.append(resolved)
    return sorted(set(tus))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="Source files (pre-commit passes these)")
    parser.add_argument(
        "--all-source",
        action="store_true",
        help="Scan all engine/source/**/*.cpp",
    )
    parser.add_argument(
        "--ccn",
        type=int,
        default=15,
        help="CCN warning threshold (lizard -C). Default: 15",
    )
    parser.add_argument(
        "--ignore-warnings",
        type=int,
        default=None,
        metavar="N",
        help="Pass lizard -i N (use -1 to report only, never fail)",
    )
    args = parser.parse_args()

    files = collect_files(args.files, args.all_source)
    if not files:
        print("lizard: no engine/source files to check")
        return 0

    cmd = [sys.executable, "-m", "lizard", "-C", str(args.ccn)]
    if args.ignore_warnings is not None:
        cmd.extend(["-i", str(args.ignore_warnings)])
    cmd.append("-w")
    cmd.extend(str(f) for f in files)

    print(f"lizard: CCN>{args.ccn} on {len(files)} file(s)", flush=True)
    completed = subprocess.run(cmd, cwd=ROOT)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
