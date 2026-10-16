#!/usr/bin/env bash
# ============================================================
# One-click multichannel launch (git-bash / MSYS2).
#
# Modes (-m): which Host process starts, and (scene 1) which IG json.
#   1  IG 直连 platform（platform_ig_*.json → 7000/7100）
#   2  IG 直连 viewhost（viewhost_ig_*.json → 8000/8100；viewhost 本地 Host）
#   3  真模拟器中继：platform → viewhost（viewhost_relay.json）→ IG（viewhost_ig_*）
#
# Scenes (-s), default 1: which IG json family. Mode still picks the Host.
#   1  无网格（按模式选 platform_ig_* / viewhost_ig_*）
#   2  接缝网格 scene_seam_grid_ig_*（json 里是 viewhost 8000/8100，只能 -m 2|3）
#
# Edit exe paths / IG list / log layout ONLY in this file.
# run_seam_grid.sh forwards here as `-m 2 -s 2`.
#
# Host / viewhost are MFC GUIs. `cmd //c start` returns immediately; a bare
# background job does not, because this bash is non-interactive (no job control).
# IG consoles stay hidden: one powershell.exe -Command starts all three.
# There is no .ps1 file. Logs: logs/ig_<name>.{out,err}.log
#
# From git-bash, or from PowerShell / cmd via Git bash:
#   scripts/run_multichannel.sh -m 2 -s 2
#   "C:\Program Files\Git\bin\bash.exe" scripts/run_multichannel.sh -m 2 -s 2
# Stop:
#   scripts/run_multichannel.sh stop
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ENGINE="$ROOT/out/build/clang-Ninja-Debug/engine/vsgEngine.exe"
PLATFORM_DIR="$ROOT/out/build/vs2019/thirdparty/sync/examples/platform/Debug"
VHOST_DIR="$ROOT/out/build/vs2019/thirdparty/sync/examples/viewhost/Debug"
VHOST_RES="$ROOT/thirdparty/sync/examples/viewhost/resources"
CFG_DIR="$ROOT/engine/resources/config"
LOG_DIR="$ROOT/logs"

usage() {
    echo "Usage:"
    echo "  $0 -m 1|2|3 [-s 1|2]   start"
    echo "      -m 1  IG→platform (7000/7100)"
    echo "      -m 2  IG→viewhost (8000/8100, local Host)"
    echo "      -m 3  platform + viewhost relay"
    echo "      -s 1  no grid (default): platform_ig_* / viewhost_ig_*"
    echo "      -s 2  seam grid: scene_seam_grid_ig_* (viewhost ports; -m 2 or 3)"
    echo "  $0 stop                stop platform, viewhost, and all vsgEngine"
}

ACTION=start
MODE=""
SCENE="1"
while [[ $# -gt 0 ]]; do
    case "$1" in
        stop)
            ACTION=stop
            shift
            ;;
        -m)
            if [[ $# -lt 2 ]]; then
                echo "[ERROR] -m needs 1, 2, or 3"
                usage
                exit 1
            fi
            MODE="$2"
            shift 2
            ;;
        -m1|-m2|-m3)
            MODE="${1#-m}"
            shift
            ;;
        -s)
            if [[ $# -lt 2 ]]; then
                echo "[ERROR] -s needs 1 or 2"
                usage
                exit 1
            fi
            SCENE="$2"
            shift 2
            ;;
        -s1|-s2)
            SCENE="${1#-s}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] unknown argument: $1"
            usage
            exit 1
            ;;
    esac
done

stop_all() {
    taskkill //IM aerovistaPlatform.exe //F >/dev/null 2>&1 || true
    taskkill //IM aerovistaViewHost.exe //F >/dev/null 2>&1 || true
    taskkill //IM vsgEngine.exe //F >/dev/null 2>&1 || true
    echo "Stopped platform, viewhost, and vsgEngine."
}

if [[ "$ACTION" == "stop" ]]; then
    stop_all
    exit 0
fi

if [[ "$MODE" != "1" && "$MODE" != "2" && "$MODE" != "3" ]]; then
    echo "[ERROR] start requires -m 1, 2, or 3"
    usage
    exit 1
fi

if [[ "$SCENE" != "1" && "$SCENE" != "2" ]]; then
    echo "[ERROR] -s needs 1 or 2 (1=no grid, 2=seam grid)"
    usage
    exit 1
fi

# scene_seam_grid_ig_* hardcodes viewhost 8000/8100, not platform 7000/7100.
if [[ "$SCENE" == "2" && "$MODE" == "1" ]]; then
    echo "[ERROR] scene 2 (scene_seam_grid_ig_*) targets viewhost 8000/8100; use -m 2 or -m 3"
    usage
    exit 1
fi

# Scene 1 keeps the mode-specific IG json. Scene 2 is the seam-grid family.
ig_prefix() {
    if [[ "$SCENE" == "2" ]]; then
        echo scene_seam_grid_ig
        return
    fi
    if [[ "$MODE" == "1" ]]; then
        echo platform_ig
    else
        echo viewhost_ig
    fi
}

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "[ERROR] not found: $1"
        echo "        Make sure the right build preset / config file exists."
        exit 1
    fi
}

require_ig_cfgs() {
    local prefix="$1"
    local name
    for name in main left right; do
        require_file "$CFG_DIR/${prefix}_${name}.json"
    done
}

install_viewhost_json() {
    local src="$1"
    require_file "$src"
    require_file "$VHOST_DIR/aerovistaViewHost.exe"
    cp "$src" "$VHOST_DIR/viewhost.json"
}

w() { cygpath -w "$1"; }

# cmd start returns immediately. A bare & does not: this bash is
# non-interactive (no job control), so an MFC process would stay in the
# foreground and the launcher would look stuck.
start_detached() {
    local dir="$1"
    local exe="$2"
    ( cd "$dir" && cmd //c start "" "$exe" )
}

start_platform() {
    require_file "$PLATFORM_DIR/aerovistaPlatform.exe"
    echo "[platform] $PLATFORM_DIR/aerovistaPlatform.exe (platform.json)"
    start_detached "$PLATFORM_DIR" aerovistaPlatform.exe
}

start_viewhost() {
    require_file "$VHOST_DIR/aerovistaViewHost.exe"
    echo "[viewhost] $VHOST_DIR/aerovistaViewHost.exe (viewhost.json)"
    start_detached "$VHOST_DIR" aerovistaViewHost.exe
}

start_igs() {
    local prefix="$1"
    local name cfg out err
    local script engine
    mkdir -p "$LOG_DIR"
    engine="$(w "$ENGINE")"
    script="\$ErrorActionPreference = 'Stop'"$'\n'
    script+="\$engine = '${engine}'"$'\n'
    for name in main left right; do
        cfg="$CFG_DIR/${prefix}_${name}.json"
        out="$(w "$LOG_DIR/ig_$name.out.log")"
        err="$(w "$LOG_DIR/ig_$name.err.log")"
        echo "[IG-$name] $cfg"
        script+="\$p = Start-Process -FilePath \$engine -ArgumentList @('-c','$(w "$cfg")') -WindowStyle Hidden -RedirectStandardOutput '${out}' -RedirectStandardError '${err}' -PassThru"$'\n'
        script+="Write-Host \"started pid=\$(\$p.Id): \$engine -c $(w "$cfg")\""$'\n'
    done
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$script"
}

require_file "$ENGINE"
stop_all

PREFIX="$(ig_prefix)"
require_ig_cfgs "$PREFIX"

case "$MODE" in
    1)
        start_platform
        start_igs "$PREFIX"
        echo
        echo "Mode 1 scene $SCENE: platform + 3 IG (${PREFIX}_* → 7000/7100)."
        ;;
    2)
        install_viewhost_json "$VHOST_RES/viewhost.json"
        start_viewhost
        start_igs "$PREFIX"
        echo
        echo "Mode 2 scene $SCENE: viewhost (local Host) + 3 IG (${PREFIX}_* → 8000/8100)."
        ;;
    3)
        require_file "$PLATFORM_DIR/aerovistaPlatform.exe"
        install_viewhost_json "$VHOST_RES/viewhost_relay.json"
        start_platform
        start_viewhost
        start_igs "$PREFIX"
        echo
        echo "Mode 3 scene $SCENE: platform → viewhost relay → 3 IG (${PREFIX}_* → 8000/8100)."
        echo "        Virtual IG connects after all 3 IGs handshake (expectedIgCount=3)."
        ;;
esac

echo "Logs under $LOG_DIR. To stop: scripts/run_multichannel.sh stop"
