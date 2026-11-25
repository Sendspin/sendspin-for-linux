#!/bin/sh
# Runs a command against a private, headless PipeWire, so the audio tests have a daemon:
#
#     ci/start-pipewire.sh ctest --test-dir build
#
# Needs dbus-daemon, pipewire, wireplumber and wpctl. Everything lives in a temporary runtime
# directory, so a desktop's own daemon is untouched.
set -eu

if [ $# -eq 0 ]; then
    echo "usage: $0 command [args...]" >&2
    exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
pids=

cleanup() {
    # shellcheck disable=SC2086
    [ -z "$pids" ] || kill $pids 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT
# sh runs the EXIT trap on exit, not on a signal
trap 'exit 130' INT TERM

fail() {
    echo "start-pipewire: $1" >&2
    tail -n 50 "$work"/*.log >&2 || true
    exit 1
}

export XDG_RUNTIME_DIR="$work/runtime"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
unset PIPEWIRE_REMOTE PIPEWIRE_RUNTIME_DIR
mkdir -m 700 "$XDG_RUNTIME_DIR"

# Config and state for the daemons only; the command keeps its own.
daemon() {
    name=$1
    shift
    XDG_CONFIG_HOME="$here" XDG_STATE_HOME="$work/state" "$@" >"$work/$name.log" 2>&1 &
    pids="$pids $!"
}

wait_for() {
    what=$1
    shift
    tries=100
    until "$@" >/dev/null 2>&1; do
        tries=$((tries - 1))
        [ "$tries" -gt 0 ] || fail "timed out waiting for $what"
        sleep 0.1
    done
}

daemon dbus dbus-daemon --session --nofork --nopidfile --address="$DBUS_SESSION_BUS_ADDRESS"
wait_for "the session bus" test -S "$XDG_RUNTIME_DIR/bus"
daemon pipewire pipewire
wait_for "pipewire" test -S "$XDG_RUNTIME_DIR/pipewire-0"
daemon wireplumber wireplumber
# The session manager is up once it has picked the null sink as the default
wait_for "a default sink" wpctl inspect @DEFAULT_AUDIO_SINK@

status=0
"$@" || status=$?
exit "$status"
