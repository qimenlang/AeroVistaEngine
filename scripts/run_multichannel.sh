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
# Channels (-c), default 3:
#   3   现有三通道（main/left/right，由 -m / -s 选 json 族）
#   10  massiveChannel/viewhost_ig_01..10（视锥 5×2，指向 viewhost 8000/8100）
#       只能 -m 2|3，且不能 -s 2（没有 10 通道接缝网格 / 平台 json）
#
# Edit exe paths / IG list / log layout ONLY in this file.
# run_seam_grid.sh forwards here as `-m 2 -s 2`.
#
# Host / viewhost are MFC GUIs. `cmd //c start` returns immediately; a bare
# background job does not, because this bash is non-interactive (no job control).
# IG consoles stay hidden: one powershell.exe -Command starts every IG.
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
    echo "  $0 -m 1|2|3 [-s 1|2] [-c 3|10]   start"
    echo "      -m 1  IG→platform (7000/7100)"
    echo "      -m 2  IG→viewhost (8000/8100, local Host)"
    echo "      -m 3  platform + viewhost relay"
    echo "      -s 1  no grid (default): platform_ig_* / viewhost_ig_*"
    echo "      -s 2  seam grid: scene_seam_grid_ig_* (viewhost ports; -m 2 or 3; -c 3 only)"
    echo "      -c 3  three channels (default): main/left/right from -m/-s"
    echo "      -c 10 ten channels: config/massiveChannel/viewhost_ig_01..10 (-m 2 or 3, not -s 2)"
    echo "  $0 stop                stop platform, viewhost, and all vsgEngine"
}

ACTION=start
MODE=""
SCENE="1"
CHANNELS="3"
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
        -c)
            if [[ $# -lt 2 ]]; then
                echo "[ERROR] -c needs 3 or 10"
                usage
                exit 1
            fi
            CHANNELS="$2"
            shift 2
            ;;
        -c3|-c10)
            CHANNELS="${1#-c}"
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

if [[ "$CHANNELS" != "3" && "$CHANNELS" != "10" ]]; then
    echo "[ERROR] -c needs 3 or 10 (3=main/left/right, 10=massiveChannel)"
    usage
    exit 1
fi

# scene_seam_grid_ig_* hardcodes viewhost 8000/8100, not platform 7000/7100.
if [[ "$SCENE" == "2" && "$MODE" == "1" ]]; then
    echo "[ERROR] scene 2 (scene_seam_grid_ig_*) targets viewhost 8000/8100; use -m 2 or -m 3"
    usage
    exit 1
fi

# massiveChannel json is the viewhost_ig family (8000/8100), 5x2 frustum, no groundGrid.
if [[ "$CHANNELS" == "10" && "$MODE" == "1" ]]; then
    echo "[ERROR] -c 10 (massiveChannel/viewhost_ig_*) targets viewhost 8000/8100; use -m 2 or -m 3"
    usage
    exit 1
fi
if [[ "$CHANNELS" == "10" && "$SCENE" == "2" ]]; then
    echo "[ERROR] -c 10 has no seam-grid json; use -s 1"
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

# Filled after argument checks: parallel arrays of log suffix and json path.
IG_NAMES=()
IG_CFGS=()

collect_ig_cfgs() {
    local name cfg
    IG_NAMES=()
    IG_CFGS=()
    if [[ "$CHANNELS" == "10" ]]; then
        local n
        for n in 01 02 03 04 05 06 07 08 09 10; do
            cfg="$CFG_DIR/massiveChannel/viewhost_ig_${n}.json"
            require_file "$cfg"
            IG_NAMES+=("$n")
            IG_CFGS+=("$cfg")
        done
        return
    fi
    local prefix
    prefix="$(ig_prefix)"
    for name in main left right; do
        cfg="$CFG_DIR/${prefix}_${name}.json"
        require_file "$cfg"
        IG_NAMES+=("$name")
        IG_CFGS+=("$cfg")
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
    local i name cfg out err
    local script engine
    mkdir -p "$LOG_DIR"
    engine="$(w "$ENGINE")"
    script="\$ErrorActionPreference = 'Stop'"$'\n'
    script+="\$engine = '${engine}'"$'\n'
    for i in "${!IG_NAMES[@]}"; do
        name="${IG_NAMES[$i]}"
        cfg="${IG_CFGS[$i]}"
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

collect_ig_cfgs
IG_COUNT="${#IG_NAMES[@]}"
if [[ "$CHANNELS" == "10" ]]; then
    IG_LABEL="massiveChannel/viewhost_ig"
else
    IG_LABEL="$(ig_prefix)"
fi

case "$MODE" in
    1)
        start_platform
        start_igs
        echo
        echo "Mode 1 scene $SCENE: platform + ${IG_COUNT} IG (${IG_LABEL}_* → 7000/7100)."
        ;;
    2)
        install_viewhost_json "$VHOST_RES/viewhost.json"
        start_viewhost
        start_igs
        echo
        echo "Mode 2 scene $SCENE: viewhost (local Host) + ${IG_COUNT} IG (${IG_LABEL}_* → 8000/8100)."
        ;;
    3)
        require_file "$PLATFORM_DIR/aerovistaPlatform.exe"
        install_viewhost_json "$VHOST_RES/viewhost_relay.json"
        start_platform
        start_viewhost
        start_igs
        echo
        echo "Mode 3 scene $SCENE: platform → viewhost relay → ${IG_COUNT} IG (${IG_LABEL}_* → 8000/8100)."
        echo "        Virtual IG connects after 3 IGs handshake (viewhost_relay.json expectedIgCount=3)."
        ;;
esac

echo "Logs under $LOG_DIR. To stop: scripts/run_multichannel.sh stop"
