#!/usr/bin/env python3
"""Run clang-tidy on engine sources (check only, no -fix).

Used by pre-commit and CI. Requires compile_commands.json from a Ninja configure
with CMAKE_EXPORT_COMPILE_COMMANDS=ON.

Env:
  AEROVISTA_TIDY_BUILD_DIR  Optional build dir containing compile_commands.json
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BUILD_CANDIDATES = (
    ROOT / "out" / "build" / "clang-Ninja",
    ROOT / "out" / "build" / "ci-debug",
    ROOT / "out" / "build" / "ci-release",
)

EXCLUDE_REL = "function/sync/Network.cpp"


def find_clang_tidy() -> str | None:
    return shutil.which("clang-tidy") or shutil.which("clang-tidy.exe")


def find_build_dir(explicit: Path | None) -> Path | None:
    if explicit is not None:
        return explicit if (explicit / "compile_commands.json").is_file() else None

    env = os.environ.get("AEROVISTA_TIDY_BUILD_DIR")
    if env:
        p = Path(env)
        return p if (p / "compile_commands.json").is_file() else None

    for candidate in BUILD_CANDIDATES:
        if (candidate / "compile_commands.json").is_file():
            return candidate

    build_root = ROOT / "out" / "build"
    if build_root.is_dir():
        for child in sorted(build_root.iterdir()):
            if (child / "compile_commands.json").is_file():
                return child
    return None


def is_excluded(path: Path) -> bool:
    return EXCLUDE_REL.replace("\\", "/") in str(path).replace("\\", "/")


def collect_files(paths: list[str], all_engine: bool) -> list[Path]:
    """Return .cpp TUs for clang-tidy.

    Headers are not tidy entry points; map foo.h -> foo.cpp when present.
    If a header has no sibling .cpp, fall back to all engine/**/*.cpp so
    header-only edits still get checked via includers.
    """
    if all_engine or not paths:
        return _all_engine_cpp()

    tus: set[Path] = set()
    unmatched_header = False

    for path in paths:
        resolved = path if Path(path).is_absolute() else (ROOT / path)
        try:
            resolved = Path(resolved).resolve()
        except OSError:
            resolved = Path(path)

        rel = str(resolved).replace("\\", "/")
        if "/engine/" not in rel:
            continue

        lower = str(resolved).lower()
        if lower.endswith((".cpp", ".cc", ".cxx")):
            if not is_excluded(resolved) and resolved.is_file():
                tus.add(resolved)
            continue

        if lower.endswith((".h", ".hpp", ".hh", ".hxx")):
            sibling = resolved.with_suffix(".cpp")
            if sibling.is_file() and not is_excluded(sibling):
                tus.add(sibling.resolve())
            else:
                unmatched_header = True

    if unmatched_header:
        return _all_engine_cpp()
    return sorted(tus)


def _all_engine_cpp() -> list[Path]:
    files: list[Path] = []
    for path in sorted((ROOT / "engine").rglob("*.cpp")):
        if not is_excluded(path):
            files.append(path.resolve())
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="Source files (pre-commit passes these)")
    parser.add_argument("--all-engine", action="store_true", help="Scan all engine/**/*.cpp")
    parser.add_argument("--build-dir", type=Path, help="CMake build dir with compile_commands.json")
    args = parser.parse_args()

    tidy = find_clang_tidy()
    if not tidy:
        print("error: clang-tidy not found in PATH", file=sys.stderr)
        print("Install LLVM and ensure clang-tidy is on PATH.", file=sys.stderr)
        return 1

    build_dir = find_build_dir(args.build_dir)
    if build_dir is None:
        print("error: compile_commands.json not found.", file=sys.stderr)
        print("Configure a Ninja preset first, e.g.:", file=sys.stderr)
        print("  cmake --preset clang-Ninja", file=sys.stderr)
        print("Or set AEROVISTA_TIDY_BUILD_DIR to that build directory.", file=sys.stderr)
        return 1

    files = collect_files(args.files, args.all_engine)
    if not files:
        print("clang-tidy: no engine sources to check")
        return 0

    cmd = [
        tidy,
        f"-p={build_dir}",
        "-header-filter=[/\\\\]engine[/\\\\]",
        "-warnings-as-errors=readability-identifier-naming",
        "--quiet",
        *[str(f) for f in files],
    ]
    print(f"clang-tidy: build_dir={build_dir}", flush=True)
    print(f"clang-tidy: checking {len(files)} file(s)", flush=True)
    completed = subprocess.run(cmd, cwd=ROOT)
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
