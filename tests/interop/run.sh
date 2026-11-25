#!/bin/sh
# Runs the interop tests: the built app against a real player (sendspin-cpp-cli) and against a
# scripted server (aiosendspin), on loopback under a private PipeWire and session bus.
#
#     tests/interop/run.sh [pytest options, such as -k cast]
#
# Needs uv, curl, what ci/start-pipewire.sh needs, and the player's libraries (Fedora: portaudio
# and avahi-compat-libdns_sd). SENDSPIN_APP names the app when it isn't build/src/sendspin-for-linux,
# SENDSPIN_CLI a player to use instead of the pinned release.
set -eu

CLI_VERSION=1.0.1
CLI_SHA256=b4b8b06789979295fbc24e858f5c546959e24bb619dab167db372138d0d1e8bd

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
cache=${XDG_CACHE_HOME:-$HOME/.cache}/sendspin-interop
mkdir -p "$cache"

if [ -z "${SENDSPIN_CLI:-}" ]; then
    [ "$(uname -m)" = x86_64 ] || { echo "run.sh: set SENDSPIN_CLI; the pinned player is x86_64" >&2; exit 2; }
    name=sendspin-cli-$CLI_VERSION-linux-x86_64
    SENDSPIN_CLI=$cache/$name/usr/local/bin/sendspin-cli
    if [ ! -x "$SENDSPIN_CLI" ]; then
        curl -fsSL -o "$cache/$name.tar.gz" \
            "https://github.com/Sendspin/sendspin-cpp-cli/releases/download/v$CLI_VERSION/$name.tar.gz"
        echo "$CLI_SHA256  $cache/$name.tar.gz" | sha256sum -c - >/dev/null
        tar -xzf "$cache/$name.tar.gz" -C "$cache"
    fi
    export SENDSPIN_CLI
fi

# requirements.txt is requirements.in locked, hashes and all:
#     uv pip compile --generate-hashes tests/interop/requirements.in -o tests/interop/requirements.txt
uv venv --quiet --allow-existing "$cache/venv"
uv pip install --quiet --python "$cache/venv" --require-hashes -r tests/interop/requirements.txt

exec ci/start-pipewire.sh "$cache/venv/bin/pytest" tests/interop -v -p no:cacheprovider "$@"
