#!/bin/sh
# Makes the release's source tarball: the tree at HEAD and, in third_party/fetched/, everything
# its build would download, so that it builds with no network (docs/SPEC.md, "Building offline").
#
#     scripts/vendor.sh [output directory, dist by default]
#
# Prints the tarball's path. Needs the build's own dependencies: a configure does the fetching,
# so the copies are exactly what the build uses.
set -eu

out=${1:-dist}
root=$(git rev-parse --show-toplevel)
version=$(sed -n 's/^project(.* VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { echo "vendor: no project(VERSION) in CMakeLists.txt" >&2; exit 1; }
name=sendspin-for-linux-$version

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$root" archive --prefix="$name/" HEAD | tar -x -C "$work"
# The metainfo's release date, whenever the tarball is built
git -C "$root" log -1 --format=%cs >"$work/$name/RELEASE_DATE"

# Never a system Boost: the tarball has to build where there is none
cmake -S "$work/$name" -B "$work/build" -G Ninja -DBUILD_TESTING=OFF \
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER >"$work/configure.log" 2>&1 ||
    { cat "$work/configure.log" >&2; exit 1; }

fetched=$work/$name/third_party/fetched
mkdir "$fetched"
for src in "$work"/build/_deps/*-src; do
    dep=$(basename "$src" -src)
    cp -a "$src" "$fetched/$dep"
    grep -q "third_party/fetched/$dep\`" "$root/NOTICES.md" ||
        { echo "vendor: NOTICES.md does not list third_party/fetched/$dep" >&2; exit 1; }
done
find "$fetched" -name .git -prune -exec rm -rf {} +
# Sample audio, most of the tree's size, that no build reads
rm -rf "$fetched/micro_flac/examples"
# Boost is every library; keep the ones Beast's build added
for lib in "$fetched"/boost/libs/*/; do
    [ -d "$work/build/_deps/boost-build/libs/$(basename "$lib")" ] || rm -rf "$lib"
done

mkdir -p "$out"
tar -C "$work" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git -C "$root" log -1 --format=%ct)" -cJf "$out/$name.tar.xz" "$name"
echo "$out/$name.tar.xz"
