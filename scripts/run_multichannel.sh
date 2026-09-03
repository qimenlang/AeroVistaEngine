#!/usr/bin/env bash
# ============================================================
# One-click multichannel launch (git-bash / MSYS2).
#   - 1 Host  : aerovistaViewHost.exe  (MFC GUI, reads viewhost.json next to exe)
#   - 3 IG    : vsgEngine.exe with viewhost_ig_main/left/right.json
# IG consoles are hidden via PowerShell; logs go to logs/ig_<name>.{out,err}.log
#
# SINGLE SOURCE OF TRUTH: run_multichannel.bat is only a thin
# Windows wrapper that forwards here. Edit exe paths / IG list /
# log layout ONLY in this file, never in the .bat.
#
# Usage:
#   scripts/run_multichannel.sh            # start
#   scripts/run_multichannel.sh stop       # stop all
#   scripts\run_multichannel.bat           # equivalent (forwards here)
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

ENGINE="$ROOT/out/build/clang-Ninja-Debug/engine/vsgEngine.exe"
VHOST_DIR="$ROOT/out/build/vs2019/thirdparty/sync/examples/viewhost/Debug"
CFG_DIR="$ROOT/engine/resources/config"
LOG_DIR="$ROOT/logs"
PS1="$ROOT/scripts/launch_vsgengine.ps1"

stop_all() {
    taskkill //IM aerovistaViewHost.exe //F >/dev/null 2>&1 || true
    taskkill //IM vsgEngine.exe //F >/dev/null 2>&1 || true
    echo "Stopped all multichannel processes."
}

if [[ "${1:-}" == "stop" ]]; then
    stop_all
    exit 0
fi

# --- Path checks (bash sees forward slashes; convert to Windows for ps1) ---
for p in "$ENGINE" "$VHOST_DIR/aerovistaViewHost.exe"; do
    if [[ ! -f "$p" ]]; then
        echo "[ERROR] not found: $p"
        echo "        Make sure the right build preset was used."
        exit 1
    fi
done
for cfg in viewhost_ig_main viewhost_ig_left viewhost_ig_right; do
    if [[ ! -f "$CFG_DIR/$cfg.json" ]]; then
        echo "[ERROR] config not found: $CFG_DIR/$cfg.json"
        exit 1
    fi
done
mkdir -p "$LOG_DIR"

w() { cygpath -w "$1"; }

echo "[Host] starting aerovistaViewHost (cwd=$VHOST_DIR, reads viewhost.json there)..."
( cd "$VHOST_DIR" && ./aerovistaViewHost.exe & )

for name in main left right; do
    cfg="$CFG_DIR/viewhost_ig_$name.json"
    outlog="$LOG_DIR/ig_$name.out.log"
    errlog="$LOG_DIR/ig_$name.err.log"
    echo "[IG-$name] $cfg (hidden console, log under logs)..."
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(w "$PS1")" \
        -Engine "$(w "$ENGINE")" \
        -Config "$(w "$cfg")" \
        -OutLog "$(w "$outlog")" \
        -ErrLog "$(w "$errlog")"
done

echo
echo "Launched 1 Host + 3 IG (no consoles; logs under $LOG_DIR)."
echo "To stop: scripts/run_multichannel.sh stop"
