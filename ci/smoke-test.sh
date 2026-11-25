#!/bin/sh
# The installed package starts: no display, a private PipeWire, and it is still running after
# ten seconds with both windows' QML loaded and an SVG drawn (--check). The licenses of what
# the binary carries came with it, where the distro keeps them.
#
#     ci/start-pipewire.sh ci/smoke-test.sh <version>
set -eu

version=$(sendspin-for-linux --version)
[ "$version" = "$1" ] || { echo "smoke-test: version $version, expected $1" >&2; exit 1; }

doc=/usr/share/doc/sendspin-for-linux
[ -d "$doc" ] || doc=/usr/share/licenses/sendspin-for-linux
for notice in LICENSE NOTICES.md \
    third_party/sendspin_cpp/LICENSE third_party/sendspin_server_cpp/LICENSE \
    third_party/sendspin_server_cpp/third_party/micro-resampler/NOTICE \
    third_party/micro_opus/lib/opus/COPYING third_party/ixwebsocket/LICENSE.txt \
    third_party/arduinojson/LICENSE.txt third_party/noise_c/LICENSE; do
    [ -s "$doc/$notice" ] || { echo "smoke-test: no $doc/$notice" >&2; exit 1; }
done

log=$(mktemp)
status=0
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software timeout 10 sendspin-for-linux --check \
    >"$log" 2>&1 || status=$?
cat "$log"
[ "$status" -eq 124 ] || { echo "smoke-test: exited with $status" >&2; exit 1; }
# A QML module or plugin the package does not depend on
if grep -E 'is not installed|is not a type|failed to load|Could not find the Qt platform' "$log"; then
    echo "smoke-test: something the app needs is missing" >&2
    exit 1
fi
