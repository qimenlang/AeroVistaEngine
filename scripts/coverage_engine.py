#!/usr/bin/env python3
"""收集并报告测试覆盖率（Clang + llvm-cov）。

配套 CMakePresets.json 的 clang-Ninja-Coverage preset：编译时插桩
-fprofile-instr-generate -fcoverage-mapping，本脚本负责 configure → build →
ctest → profdata 合并 → llvm-cov 报告。

统计范围与 lizard_engine.py / clang_tidy_engine.py 对齐：
  engine/source + thirdparty/sync/src + thirdparty/config/src

前置：C:/Program Files/LLVM/bin/ 下的 clang/clang++/llvm-profdata/llvm-cov
（与 clang-Ninja-Coverage preset 同一套工具链）。

Examples:
  # 全量：configure + build + 测试 + 出 HTML 报告到 out/coverage/html
  python scripts/coverage_engine.py
  # 只重新跑测试 + 出报告（已 configure/build 过）
  python scripts/coverage_engine.py --no-configure --no-build
  # 只出文本汇总，不生成 HTML
  python scripts/coverage_engine.py --summary-only
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRESET = "clang-Ninja-Coverage"
BUILD_DIR = ROOT / "out" / "build" / PRESET
COVERAGE_DIR = ROOT / "out" / "coverage"
TEST_TARGET = "vsgEngineTests"

# llvm-cov --sources 过滤：只统计生产源码，排除第三方（vsg/cigi/catch2 等）。
# 用正则，锚定相对路径根。
SOURCE_FILTERS = [
    "engine/source/.*",
    "thirdparty/sync/src/.*",
    "thirdparty/config/src/.*",
]


def find_llvm_tool(name: str) -> str:
    """从 clang++ 所在目录找 llvm 工具，确保与编译器同版本。"""
    clangxx = shutil.which("clang++") or r"C:/Program Files/LLVM/bin/clang++.exe"
    tool = Path(clangxx).with_name(name)
    if not tool.exists():
        # 退回 PATH 查找
        found = shutil.which(name)
        if not found:
            sys.exit(f"coverage: 找不到 {name}（与 {clangxx} 同目录或 PATH）")
        return found
    return str(tool)


def run(cmd: list[str], *, cwd: Path | None = None, env: dict | None = None) -> None:
    printable = " ".join(str(c) for c in cmd)
    where = f" (cwd={cwd})" if cwd else ""
    print(f"> {printable}{where}", flush=True)
    result = subprocess.run(cmd, cwd=cwd, env=env)
    if result.returncode != 0:
        sys.exit(f"coverage: 命令失败 rc={result.returncode}: {printable}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-configure", action="store_true", help="跳过 cmake configure")
    parser.add_argument("--no-build", action="store_true", help="跳过 cmake build")
    parser.add_argument(
        "--test-filter",
        default=None,
        help="Catch2 tag/expression，只跑部分用例（如 [CIGI-ownship-lla]）",
    )
    parser.add_argument(
        "--summary-only",
        action="store_true",
        help="只出文本汇总，不生成 HTML 报告",
    )
    parser.add_argument(
        "--keep-profraw",
        action="store_true",
        help="保留合并后的 .profraw（默认清理）",
    )
    args = parser.parse_args()

    profdata = shutil.which("llvm-profdata") or find_llvm_tool("llvm-profdata.exe")
    lcov = shutil.which("llvm-cov") or find_llvm_tool("llvm-cov.exe")

    COVERAGE_DIR.mkdir(parents=True, exist_ok=True)
    profraw_glob = "codecov_*.profraw"
    merged = COVERAGE_DIR / "coverage.profdata"

    # 清旧 profraw，避免混入上次跑的数据
    for old in COVERAGE_DIR.glob(profraw_glob):
        old.unlink()
    if merged.exists():
        merged.unlink()

    if not args.no_configure:
        run(["cmake", "--preset", PRESET], cwd=ROOT)
    if not args.no_build:
        run(["cmake", "--build", str(BUILD_DIR)], cwd=ROOT)

    # 跑测试：profraw 写到 COVERAGE_DIR，%p=pid 防多进程覆盖，%m=模块名分组
    test_env = os.environ.copy()
    test_env["LLVM_PROFILE_FILE"] = str(COVERAGE_DIR / "codecov_%p_%m.profraw")
    ctest_cmd = ["ctest", "--test-dir", str(BUILD_DIR), "--output-on-failure"]
    if args.test_filter:
        ctest_cmd += ["-R", args.test_filter]
    run(ctest_cmd, env=test_env)

    profraws = sorted(COVERAGE_DIR.glob(profraw_glob))
    if not profraws:
        sys.exit(f"coverage: 未找到 {profraw_glob}，测试可能没跑或插桩未生效")
    run([profdata, "merge", "-o", str(merged), *[str(p) for p in profraws]])

    exe_name = TEST_TARGET + (".exe" if os.name == "nt" else "")
    test_exe = BUILD_DIR / exe_name
    if not test_exe.exists():
        sys.exit(f"coverage: 找不到测试可执行 {test_exe}")

    sources_args = []
    for pat in SOURCE_FILTERS:
        sources_args += ["--sources", pat]

    # 文本汇总
    run([lcov, "report", str(test_exe), f"-instr-profile={merged}", *sources_args])

    if not args.summary_only:
        html_dir = COVERAGE_DIR / "html"
        if html_dir.exists():
            shutil.rmtree(html_dir)
        run(
            [
                lcov,
                "show",
                str(test_exe),
                f"-instr-profile={merged}",
                "--format=html",
                f"-output-dir={html_dir}",
                *sources_args,
            ]
        )
        print(f"\ncoverage: HTML 报告 -> {html_dir / 'index.html'}")

    if not args.keep_profraw:
        for p in profraws:
            p.unlink()

    print(f"coverage: 汇总数据 -> {merged}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
