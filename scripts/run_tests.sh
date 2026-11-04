#!/usr/bin/env bash
# 依次运行 config / sync / engine 三个测试 exe。
# 同一份文本一边在当前窗口打印，一边写入仓库根目录 test_out.txt（已在 .gitignore）。
#
# 用法（git-bash / MSYS2，任意工作目录）：
#   scripts/run_tests.sh
#   scripts/run_tests.sh --only config
#   scripts/run_tests.sh --build-dir out/build/clang-Ninja-Debug --log test_out.txt
#
# 从 PowerShell / cmd：
#   bash scripts/run_tests.sh
#   "C:\Program Files\Git\bin\bash.exe" scripts/run_tests.sh
#
# stdout 经管道离开控制台后，C 运行时改为全缓冲：窗口里仍出字，但是成块，
# Catch2 颜色也会关掉。进程退出时缓冲冲完，文件是完整日志。

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="out/build/clang-Ninja-Debug"
LOG="test_out.txt"
ONLY=()

usage() {
    echo "Usage:"
    echo "  scripts/run_tests.sh [--only config|sync|engine] [--build-dir DIR] [--log FILE]"
    echo "  no args = run config, sync, and engine"
    echo "  --only may be repeated, or comma-separated: --only config,sync"
    echo "  relative --build-dir / --log are resolved from the repo root"
}

die() {
    echo "[ERROR] $*" >&2
    usage >&2
    exit 1
}

add_only() {
    local raw part
    raw="${1// /}"
    IFS=',' read -ra parts <<< "$raw"
    for part in "${parts[@]}"; do
        [[ -n "$part" ]] || continue
        case "$part" in
            config|sync|engine) ONLY+=("$part") ;;
            *) die "unknown suite: $part" ;;
        esac
    done
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only|-o|-Only)
            [[ $# -ge 2 ]] || die "--only needs config, sync, or engine"
            add_only "$2"
            shift 2
            ;;
        --build-dir|-BuildDir)
            [[ $# -ge 2 ]] || die "--build-dir needs a path"
            BUILD_DIR="$2"
            shift 2
            ;;
        --log|-Log)
            [[ $# -ge 2 ]] || die "--log needs a path"
            LOG="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

if [[ ${#ONLY[@]} -eq 0 ]]; then
    ONLY=(config sync engine)
fi

to_unix_slash() {
    printf '%s' "${1//\\//}"
}

resolve_path() {
    local path
    path="$(to_unix_slash "$1")"
    case "$path" in
        /*|[A-Za-z]:*) printf '%s' "$path" ;;
        *) printf '%s' "$ROOT/$path" ;;
    esac
}

BUILD="$(resolve_path "$BUILD_DIR")"
LOG_PATH="$(resolve_path "$LOG")"

want() {
    local name="$1" item
    for item in "${ONLY[@]}"; do
        [[ "$item" == "$name" ]] && return 0
    done
    return 1
}

say() {
    printf '%s\n' "$*" | tee -a "$LOG_PATH"
}

NAMES=(config sync engine)
RELS=(
    "thirdparty/config/tests/aerovistaConfigTests.exe"
    "thirdparty/sync/tests/aerovistaSyncTests.exe"
    "engine/Tests/vsgEngineTests.exe"
)

mkdir -p "$(dirname "$LOG_PATH")"
: > "$LOG_PATH"

say "log: $LOG_PATH"
say "build: $BUILD"

failed=()
for i in "${!NAMES[@]}"; do
    name="${NAMES[$i]}"
    want "$name" || continue

    exe="$BUILD/${RELS[$i]}"
    banner="======== $name ========"
    say ""
    say "$banner"
    say "$exe"

    if [[ ! -f "$exe" ]]; then
        say "MISSING: $exe"
        failed+=("$name")
        continue
    fi

    # pipefail 让 $? 反映测试进程；不用 set -e，失败后继续跑后面的 exe。
    "$exe" 2>&1 | tee -a "$LOG_PATH"
    code=${PIPESTATUS[0]}

    say "-------- $name exit $code --------"
    if [[ "$code" -ne 0 ]]; then
        failed+=("$name")
    fi
done

if [[ ${#failed[@]} -gt 0 ]]; then
    joined=$(printf '%s, ' "${failed[@]}")
    say "FAILED: ${joined%, }"
    exit 1
fi

say "ALL PASSED"
exit 0
