#!/bin/sh
# The installed package starts: no display, a private PipeWire, and it is still running after
# ten seconds with its windows' QML loaded.
#
#     ci/start-pipewire.sh ci/smoke-test.sh <version>
set -eu

version=$(sendspin-for-linux --version)
[ "$version" = "$1" ] || { echo "smoke-test: version $version, expected $1" >&2; exit 1; }

log=$(mktemp)
status=0
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software timeout 10 sendspin-for-linux >"$log" 2>&1 ||
    status=$?
cat "$log"
[ "$status" -eq 124 ] || { echo "smoke-test: exited with $status" >&2; exit 1; }
# A QML module or plugin the package does not depend on
if grep -E 'is not installed|is not a type|failed to load|Could not find the Qt platform' "$log"; then
    echo "smoke-test: something the app needs is missing" >&2
    exit 1
fi
