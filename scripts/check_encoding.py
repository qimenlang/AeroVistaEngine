#!/usr/bin/env python3
"""Two-layer encoding check for source files (CONTRIBUTING.md §源码编码).

Layer 1 (bytes):  reject files that are not valid UTF-8 (cannot be decoded).
Layer 2 (content): reject files that decode as UTF-8 but contain mojibake
    marker characters — modifier letters / combining diacritics that only
    appear in practice as double-encoding damage (e.g. 'ʱ', '��').

Used by pre-commit (encoding-check hook) and CI.
Usage:
  python scripts/check_encoding.py [files...]
  python scripts/check_encoding.py --all
Exit non-zero when any checked file fails either layer.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Source file extensions that must be UTF-8.
CHECKED_SUFFIXES = (
    ".h", ".hpp", ".hh", ".hxx",
    ".cpp", ".cc", ".cxx",
    ".c",
    ".md",
    ".json",
    ".cmake",
    ".py",
    ".yml", ".yaml",
    ".txt",
)

# Layer-2 (mojibake marker) check applies only to code files. Docs (.md etc.)
# legitimately quote mojibake examples, so content-layer check would false-positive.
CONTENT_CHECK_SUFFIXES = (
    ".h", ".hpp", ".hh", ".hxx",
    ".cpp", ".cc", ".cxx",
    ".c",
)

# Layer-2 mojibake markers: modifier letters (U+02B0–U+02FF), combining
# diacritics (U+0300–U+036F, U+1AB0–U+1AFF), and the CJK-replacement-ish
# characters that appear when GBK bytes are decoded as UTF-8 ('��' etc.).
# Deliberately narrow: real text rarely uses these ranges.
_MOJIBAKE_RE = None


def _mojibake_re():
    global _MOJIBAKE_RE
    if _MOJIBAKE_RE is None:
        import re
        _MOJIBAKE_RE = re.compile(
            "[\u02B0-\u02FF"      # Spacing Modifier Letters
            "\u0300-\u036F"        # Combining Diacritical Marks
            "\u1AB0-\u1AFF"        # Combining Diacritical Marks Extended
            "\uFFFD"               # replacement char (decode loss)
            "]"
        )
    return _MOJIBAKE_RE


def check_file(path: Path) -> list[str]:
    """Return list of problems for one file (empty = ok)."""
    problems: list[str] = []
    try:
        raw = path.read_bytes()
    except OSError as exc:
        return [f"{path}: cannot read ({exc})"]

    # Layer 1: must be valid UTF-8.
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        problems.append(f"{path}: NOT valid UTF-8 (byte {exc.start})")
        return problems

    # Layer 2 (code files only): must not contain mojibake markers.
    if path.suffix.lower() in CONTENT_CHECK_SUFFIXES:
        for line_no, line in enumerate(text.splitlines(), 1):
            if _mojibake_re().search(line):
                problems.append(f"{path}:{line_no}: mojibake marker: {line[:80]!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="Files to check")
    parser.add_argument("--all", action="store_true", help="Scan all project sources (engine + thirdparty/sync)")
    args = parser.parse_args()

    if args.all:
        files = sorted(
            p
            for root in (ROOT / "engine" / "source", ROOT / "engine" / "Tests", ROOT / "thirdparty" / "sync")
            for p in root.rglob("*")
            if p.suffix.lower() in CHECKED_SUFFIXES and p.is_file()
        )
    elif args.files:
        files = [Path(f) if Path(f).is_absolute() else ROOT / f for f in args.files]
    else:
        parser.error("pass files or --all")

    problems: list[str] = []
    for path in files:
        if not path.is_file():
            problems.append(f"{path}: missing")
            continue
        problems.extend(check_file(path))

    if problems:
        print(f"encoding check: {len(problems)} problem(s)")
        for p in problems:
            print(" ", p)
        return 1

    print(f"encoding check: ok ({len(files)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
