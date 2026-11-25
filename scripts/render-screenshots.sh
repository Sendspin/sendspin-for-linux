#!/bin/sh
# Draws the metainfo's screenshots, data/screenshots/picker.png and settings.png, offscreen and
# at twice their size: Fusion on a light palette, stand-in speakers on the loopback interface, a
# private PipeWire.
#
#     scripts/render-screenshots.sh [build directory, build by default]
#
# Needs what ci/start-pipewire.sh needs. The text is drawn in whatever font this machine gives
# Qt, so two machines' pictures differ in that.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$root/build}
cmake --build "$build" --target render-screenshots

# ci/'s daemon, with a name for this PC's output that suits a picture
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$root/ci" "$work/ci"
sed -i 's/"CI null sink"/"Built-in Audio"/' "$work/ci/pipewire/pipewire.conf.d/null-sink.conf"

QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_SCALE_FACTOR=2 \
    QT_QUICK_CONTROLS_STYLE=Fusion QT_QPA_PLATFORMTHEME= \
    XDG_CONFIG_DIRS="$work/none" DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent \
    "$work/ci/start-pipewire.sh" "$build/src/render-screenshots" "$root/data/screenshots"
