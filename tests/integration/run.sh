#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Headless biome scenarios. Usage (under dbus-run-session):
#   run.sh <biome> <client> <shim.so> <scenario>
set -u

BIOME=$1 CLIENT=$2 SHIM=$3 SCENARIO=$4

RT=$(mktemp -d /tmp/biome-itest.XXXXXX)
chmod 700 "$RT"
mkdir -p "$RT/cfg"
export XDG_RUNTIME_DIR=$RT XDG_CONFIG_HOME=$RT/cfg
unset WAYLAND_DISPLAY DISPLAY
ulimit -c 0

BIOME_PID=
CLIENT_PIDS=()

cleanup() {
    for p in "${CLIENT_PIDS[@]}"; do kill "$p" 2>/dev/null; done
    [ -n "$BIOME_PID" ] && kill -9 "$BIOME_PID" 2>/dev/null
    if [ "${KEEP:-0}" = 1 ]; then echo "logs kept in $RT"; else rm -rf "$RT"; fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    echo "--- biome.log (tail)"
    tail -n 40 "$RT/biome.log" 2>/dev/null
    for f in "$RT"/client-*.log; do [ -e "$f" ] && { echo "--- $f"; cat "$f"; }; done
    exit 1
}

# wait_for <file> <regex> [timeout seconds]
wait_for() {
    local deadline=$(( $(date +%s%N) / 1000000 + ${3:-5} * 1000 ))
    while ! grep -Eq "$2" "$1" 2>/dev/null; do
        [ "$(( $(date +%s%N) / 1000000 ))" -ge "$deadline" ] && return 1
        sleep 0.05
    done
}

# start_biome <outputs> [biome args...]
start_biome() {
    local outputs=$1; shift
    LD_PRELOAD=$SHIM WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=$outputs WLR_RENDERER=pixman \
        WLR_LIBINPUT_NO_DEVICES=1 "$BIOME" "$@" > "$RT/biome.log" 2>&1 &
    BIOME_PID=$!
    wait_for "$RT/biome.log" "Running Biome on WAYLAND_DISPLAY=" 10 || fail "biome did not start"
    export WAYLAND_DISPLAY=$(sed -n 's/.*Running Biome on WAYLAND_DISPLAY=//p' "$RT/biome.log" | head -1)
}

# client <name> <args...>: background client logging to client-<name>.log
client() {
    local name=$1; shift
    "$CLIENT" "$@" > "$RT/client-$name.log" 2>&1 &
    CLIENT_PIDS+=($!)
}

# Exit biome with SIGTERM; teardown must not abort.
stop_biome() {
    kill -0 "$BIOME_PID" 2>/dev/null || fail "biome died"
    kill -TERM "$BIOME_PID"
    local i
    for i in $(seq 100); do kill -0 "$BIOME_PID" 2>/dev/null || break; sleep 0.1; done
    wait "$BIOME_PID"
    local status=$?
    BIOME_PID=
    [ "$status" = 0 ] || fail "biome exited with $status after SIGTERM"
}

case "$SCENARIO" in
startup-exit)
    # Exits with the -s command, through teardown with live clients of each kind.
    printf '%s win a > %s/client-win.log &\n%s panel > %s/client-panel.log &\n%s lock > %s/client-lock.log &\nsleep 1\nexit 7\n' \
        "$CLIENT" "$RT" "$CLIENT" "$RT" "$CLIENT" "$RT" > "$RT/startup.sh"
    start_biome 1 -s "sh $RT/startup.sh"
    for i in $(seq 150); do kill -0 "$BIOME_PID" 2>/dev/null || break; sleep 0.1; done
    wait "$BIOME_PID"
    status=$?
    BIOME_PID=
    [ "$status" = 7 ] || fail "biome exited with $status, expected 7"
    grep -q locked "$RT/client-lock.log" || fail "lock never confirmed"
    ;;
sigterm)
    start_biome 2
    client win win a
    client panel panel
    wait_for "$RT/client-win.log" ready || fail "window not mapped"
    wait_for "$RT/client-panel.log" ready || fail "panel not configured"
    client lock lock
    wait_for "$RT/client-lock.log" locked || fail "not locked"
    stop_biome
    ;;
remap)
    start_biome 1
    client a win a remap
    client b win b ssd
    wait_for "$RT/client-a.log" ready || fail "window a not remapped"
    wait_for "$RT/client-b.log" ready || fail "window b not mapped"
    grep -q "decoration server-side" "$RT/client-b.log" || fail "window b not server-decorated"
    stop_biome
    ;;
lock-outputs-off)
    start_biome 1
    "$CLIENT" power off || fail "power off"
    client lock lock
    wait_for "$RT/client-lock.log" locked 3 || fail "locked never sent with outputs off"
    "$CLIENT" power on || fail "power on"
    stop_biome
    ;;
lock-unplug)
    # Lock focus with a window under it, then unplug the focused lock surface's output.
    start_biome 2
    client win win a
    wait_for "$RT/client-win.log" "enter window" || fail "window not focused"
    client lock lock
    wait_for "$RT/client-lock.log" locked || fail "not locked"
    wait_for "$RT/client-lock.log" "enter HEADLESS" || fail "lock surface not focused"
    focused=$(sed -n 's/^enter //p' "$RT/client-lock.log" | tail -1)
    touch "$RT/key-30"
    wait_for "$RT/client-lock.log" "^key 30" || fail "key not delivered to lock surface"
    touch "$RT/unplug-$focused"
    wait_for "$RT/client-lock.log" "output-removed $focused" || fail "output not removed"
    other=HEADLESS-1
    [ "$focused" = HEADLESS-1 ] && other=HEADLESS-2
    wait_for "$RT/client-lock.log" "enter $other" || fail "focus not moved to $other"
    touch "$RT/key-30"
    for i in $(seq 100); do [ "$(grep -c '^key 30' "$RT/client-lock.log")" -ge 2 ] && break; sleep 0.05; done
    [ "$(grep -c '^key 30' "$RT/client-lock.log")" -ge 2 ] || fail "key lost after unplug"
    grep -q "^key" "$RT/client-win.log" && fail "window received a key while locked"
    stop_biome
    ;;
workspaces)
    # ext-workspace ids are the org.biome.Workspaces numbers.
    start_biome 1
    "$CLIENT" workspaces 2 > "$RT/client-ws.log" || fail "workspace client failed"
    head -4 "$RT/client-ws.log" | grep -qx "workspace id=0 name=1 active" || fail "workspace 0 not first/active"
    grep -qx "workspace id=3 name=4" "$RT/client-ws.log" || fail "workspace 3 missing"
    sed -n '/after-activate/,$p' "$RT/client-ws.log" | grep -qx "workspace id=2 name=3 active" ||
        fail "activate did not switch to workspace 2"
    stop_biome
    ;;
*)
    echo "unknown scenario $SCENARIO"
    exit 2
    ;;
esac
echo "PASS: $SCENARIO"
