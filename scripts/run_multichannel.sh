#!/usr/bin/env bash
# ============================================================
# One-click multichannel launch (git-bash / MSYS2).
#
# Modes (-m):
#   1  IG 直连 platform（platform_ig_*.json → 7000/7100）
#   2  IG 直连 viewhost（viewhost_ig_*.json → 8000/8100；viewhost 本地 Host）
#   3  真模拟器中继：platform → viewhost（viewhost_relay.json）→ IG（viewhost_ig_*）
#
# SINGLE SOURCE OF TRUTH: run_multichannel.bat is only a thin
# Windows wrapper that forwards here. Edit exe paths / IG list /
# log layout ONLY in this file, never in the .bat.
#
# Usage:
#   scripts/run_multichannel.sh -m 1
#   scripts/run_multichannel.sh -m 2
#   scripts/run_multichannel.sh -m 3
#   scripts/run_multichannel.sh stop
#   scripts\run_multichannel.bat -m 1
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ENGINE="$ROOT/out/build/clang-Ninja-Debug/engine/vsgEngine.exe"
PLATFORM_DIR="$ROOT/out/build/vs2019/thirdparty/sync/examples/platform/Debug"
VHOST_DIR="$ROOT/out/build/vs2019/thirdparty/sync/examples/viewhost/Debug"
VHOST_RES="$ROOT/thirdparty/sync/examples/viewhost/resources"
CFG_DIR="$ROOT/engine/resources/config"
LOG_DIR="$ROOT/logs"
PS1="$ROOT/scripts/launch_vsgengine.ps1"

usage() {
    echo "Usage:"
    echo "  $0 -m 1|2|3     start (1=IG→platform, 2=IG→viewhost, 3=platform+viewhost relay)"
    echo "  $0 stop         stop platform, viewhost, and all vsgEngine"
}

ACTION=start
MODE=""
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

start_platform() {
    require_file "$PLATFORM_DIR/aerovistaPlatform.exe"
    echo "[platform] $PLATFORM_DIR/aerovistaPlatform.exe (platform.json)"
    ( cd "$PLATFORM_DIR" && ./aerovistaPlatform.exe & )
}

start_viewhost() {
    require_file "$VHOST_DIR/aerovistaViewHost.exe"
    echo "[viewhost] $VHOST_DIR/aerovistaViewHost.exe (viewhost.json)"
    ( cd "$VHOST_DIR" && ./aerovistaViewHost.exe & )
}

start_igs() {
    local prefix="$1"
    local name cfg
    mkdir -p "$LOG_DIR"
    for name in main left right; do
        cfg="$CFG_DIR/${prefix}_${name}.json"
        echo "[IG-$name] $cfg"
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(w "$PS1")" \
            -Engine "$(w "$ENGINE")" \
            -Config "$(w "$cfg")" \
            -OutLog "$(w "$LOG_DIR/ig_$name.out.log")" \
            -ErrLog "$(w "$LOG_DIR/ig_$name.err.log")"
    done
}

require_file "$ENGINE"
stop_all

case "$MODE" in
    1)
        require_ig_cfgs platform_ig
        start_platform
        start_igs platform_ig
        echo
        echo "Mode 1: platform + 3 IG (platform_ig_* → 7000/7100)."
        ;;
    2)
        require_ig_cfgs viewhost_ig
        install_viewhost_json "$VHOST_RES/viewhost.json"
        start_viewhost
        start_igs viewhost_ig
        echo
        echo "Mode 2: viewhost (local Host) + 3 IG (viewhost_ig_* → 8000/8100)."
        ;;
    3)
        require_ig_cfgs viewhost_ig
        require_file "$PLATFORM_DIR/aerovistaPlatform.exe"
        install_viewhost_json "$VHOST_RES/viewhost_relay.json"
        start_platform
        start_viewhost
        start_igs viewhost_ig
        echo
        echo "Mode 3: platform → viewhost relay → 3 IG (viewhost_ig_* → 8000/8100)."
        echo "        Virtual IG connects after all 3 IGs handshake (expectedIgCount=3)."
        ;;
esac

echo "Logs under $LOG_DIR. To stop: scripts/run_multichannel.sh stop"
