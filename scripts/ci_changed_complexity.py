#!/usr/bin/env python3
"""CI helper: run clang-tidy + lizard on files changed vs a git base.

Environment (GitHub Actions):
  GITHUB_EVENT_NAME          pull_request | push | ...
  GITHUB_BASE_SHA            PR base commit (optional; else pull_request.base.sha)
  GITHUB_EVENT_BEFORE        push before SHA (all-zero on new branch)

Exit non-zero if either tool fails. No engine files in the diff → success.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TIDY_RE = re.compile(r"^engine/.+\.(cpp|cc|cxx|h|hpp|hh|hxx)$", re.I)
LIZARD_RE = re.compile(r"^engine/source/.+\.(cpp|cc|cxx)$", re.I)


def git_output(*args: str) -> str:
    # UTF-8: repo paths may be non-ASCII; Windows default locale can be GBK.
    return subprocess.check_output(
        ["git", "-c", "core.quotepath=false", *args],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="surrogateescape",
    )


def resolve_base(explicit: str | None) -> str:
    if explicit:
        return explicit
    if os.environ.get("GITHUB_EVENT_NAME") == "pull_request":
        base = os.environ.get("GITHUB_BASE_SHA") or os.environ.get("GITHUB_EVENT_PULL_REQUEST_BASE_SHA")
        if base:
            return base
    before = os.environ.get("GITHUB_EVENT_BEFORE", "")
    if before and not re.fullmatch(r"0+", before):
        return before
    return "HEAD~1"


def changed_files(base: str) -> list[str]:
    out = git_output("diff", "--name-only", "--diff-filter=ACMR", f"{base}...HEAD")
    return [line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="Git base commit/ref for diff")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "out" / "build" / "ci-clang",
        help="compile_commands.json build dir for clang-tidy",
    )
    parser.add_argument("--ccn", type=int, default=15, help="lizard CCN threshold")
    args = parser.parse_args()

    base = resolve_base(args.base)
    print(f"complexity gates: diff {base}...HEAD", flush=True)
    try:
        changed = changed_files(base)
    except subprocess.CalledProcessError as exc:
        print(f"error: git diff failed: {exc}", file=sys.stderr)
        return 1

    tidy_files = [f for f in changed if TIDY_RE.match(f)]
    lizard_files = [f for f in changed if LIZARD_RE.match(f)]
    print(f"changed paths: {len(changed)}; tidy={len(tidy_files)}; lizard={len(lizard_files)}", flush=True)

    rc = 0
    if tidy_files:
        cmd = [
            sys.executable,
            str(ROOT / "scripts" / "clang_tidy_engine.py"),
            f"--build-dir={args.build_dir}",
            *tidy_files,
        ]
        print("+", " ".join(cmd), flush=True)
        completed = subprocess.run(cmd, cwd=ROOT)
        rc = completed.returncode or rc
    else:
        print("clang-tidy: no changed engine sources", flush=True)

    if lizard_files:
        cmd = [
            sys.executable,
            str(ROOT / "scripts" / "lizard_engine.py"),
            f"--ccn={args.ccn}",
            *lizard_files,
        ]
        print("+", " ".join(cmd), flush=True)
        completed = subprocess.run(cmd, cwd=ROOT)
        rc = completed.returncode or rc
    else:
        print("lizard: no changed engine/source sources", flush=True)

    return rc


if __name__ == "__main__":
    sys.exit(main())
