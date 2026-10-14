#!/usr/bin/env bash
# ============================================================
# Configure and build all targets for the local CMake presets.
#
#   clang-Ninja-Debug   Ninja, single-config (CMAKE_BUILD_TYPE=Debug)
#   vs2019              Visual Studio 2019. Multi-config; this script
#                       builds Debug through the existing build preset
#                       vs2019-debug (all targets, including the MFC
#                       platform / viewhost examples).
#
# vs2019 names cl.exe as the compiler. git-bash does not have the VS
# toolset on PATH, so that step loads vcvars64.bat via vswhere.
#
# Usage (git-bash / MSYS2):
#   scripts/config_build.sh
#   scripts/config_build.sh clang-Ninja-Debug
#   scripts/config_build.sh vs2019
#   scripts\config_build.bat
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
    echo "Usage:"
    echo "  scripts/config_build.sh [clang-Ninja-Debug] [vs2019]"
    echo "  no args = configure and build all targets for both presets"
    echo "  vs2019 uses build preset vs2019-debug (Debug, all targets)"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

PRESETS=("$@")
if [[ ${#PRESETS[@]} -eq 0 ]]; then
    PRESETS=(clang-Ninja-Debug vs2019)
fi

require_cmake() {
    if ! command -v cmake >/dev/null 2>&1; then
        echo "[ERROR] cmake not on PATH"
        exit 1
    fi
}

config_build_clang() {
    echo "[config] cmake --preset clang-Ninja-Debug"
    ( cd "$ROOT" && cmake --preset clang-Ninja-Debug )
    echo "[build] clang-Ninja-Debug (all targets)"
    ( cd "$ROOT" && cmake --build "out/build/clang-Ninja-Debug" --parallel )
}

vs2019_vcvars() {
    local vswhere install unix vcvars
    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ ! -f "$vswhere" ]]; then
        echo "[ERROR] vswhere not found: $vswhere"
        exit 1
    fi
    install="$("$vswhere" -version "[16.0,17.0)" -latest -products "*" \
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
        -property installationPath | tr -d '\r')"
    if [[ -z "$install" ]]; then
        echo "[ERROR] Visual Studio 2019 with the MSVC toolset was not found"
        exit 1
    fi
    unix="$(cygpath -u "$install")"
    vcvars="$unix/VC/Auxiliary/Build/vcvars64.bat"
    if [[ ! -f "$vcvars" ]]; then
        echo "[ERROR] vcvars64.bat not found: $vcvars"
        exit 1
    fi
    cygpath -w "$vcvars"
}

config_build_vs2019() {
    local vcvars root_win runner win_runner status
    if ! command -v cygpath >/dev/null 2>&1; then
        echo "[ERROR] cygpath not found; run this script from git-bash"
        exit 1
    fi
    vcvars="$(vs2019_vcvars)"
    root_win="$(cygpath -w "$ROOT")"
    runner="$(mktemp "${TMPDIR:-/tmp}/aerovista-config-build.XXXXXX.bat")"
    # Unquoted heredoc so the two paths expand; %ERRORLEVEL% is left for cmd.
    cat > "$runner" <<EOF
@echo off
call "$vcvars" >nul
cd /d "$root_win"
cmake --preset vs2019 || exit /b 1
cmake --build --preset vs2019-debug --parallel
exit /b %ERRORLEVEL%
EOF
    win_runner="$(cygpath -w "$runner")"
    echo "[config] cmake --preset vs2019"
    echo "[build] cmake --build --preset vs2019-debug (all targets)"
    # MSYS_NO_PATHCONV keeps /c and the Windows path intact. The extra
    # quotes survive cmd's /c quote-stripping when the path contains spaces.
    set +e
    MSYS_NO_PATHCONV=1 cmd.exe /c "\"$win_runner\""
    status=$?
    set -e
    rm -f "$runner"
    if [[ $status -ne 0 ]]; then
        exit "$status"
    fi
}

require_cmake

for preset in "${PRESETS[@]}"; do
    case "$preset" in
        clang-Ninja-Debug)
            config_build_clang
            ;;
        vs2019)
            config_build_vs2019
            ;;
        *)
            echo "[ERROR] unknown preset: $preset"
            usage
            exit 1
            ;;
    esac
done

echo "config & build finished: ${PRESETS[*]}"
