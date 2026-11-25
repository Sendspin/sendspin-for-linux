#!/bin/sh
# A clean machine set up by the README's own lines installs the package from the repositories
# of ci/repo-fixture.sh and upgrades it, and refuses an index that was changed or signed with
# another key. As root; needs python3 to serve them, and wget for apt.
#
#     ci/repo-test.sh <site> <site signed with another key> <version>
set -eu

version=$3
url=http://127.0.0.1:8000
package=sendspin-for-linux
# Where the README's lines fetch from
published=https://sendspin.github.io/sendspin-for-linux

fail() {
    echo "repo-test: $*" >&2
    exit 1
}

# One address, and what it serves is swapped under it
served=$(mktemp -d)
cp -r "$1" "$served/good"
cp -r "$1" "$served/changed"
cp -r "$2" "$served/other-key"
ln -s good "$served/now"
python3 -m http.server --bind 127.0.0.1 --directory "$served/now" 8000 >/dev/null 2>&1 &
server=$!
trap 'kill $server' EXIT
serve() {
    ln -sfn "$1" "$served/now"
}

# Runs the README's blocks marked "repo-setup: <name>" against this site, up to their last
# line, which has to be the install given: the test installs an old version first instead.
# The key check between them is not marked: it needs gpg, and refresh() tests the key below
readme_setup() {
    block=$(awk -v marker="<!-- repo-setup: $1 -->" '
        { sub(/^ +/, "") }
        $0 == marker { found = 1; next }
        found && /^```/ { if (inside) found = 0; inside = !inside; next }
        inside { print }' "$(dirname "$0")/../README.md")
    [ "$(printf '%s\n' "$block" | tail -n 1)" = "$2" ] ||
        fail "the README's $1 block does not end with: $2"
    setup=$(printf '%s\n' "$block" | sed -e '$d' -e "s|$published|$url|g")
    case "$setup" in
        *"$url"*) ;;
        *) fail "the README's $1 block does not fetch from $published" ;;
    esac
    # The server may not be up yet
    for _ in 1 2 3 4 5; do
        sh -euc "sudo() { \"\$@\"; }
$setup" && return
        sleep 1
    done
    fail "the README's $1 block failed"
}

. /etc/os-release
case "$ID" in
    debian | ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        readme_setup apt "sudo apt install $package"
        # A signature that fails is only a warning to apt-get update, without this
        refresh() {
            apt-get update -o APT::Update::Error-Mode=any
        }
        refresh
        apt-get install -y --no-install-recommends "$package=0"
        apt-get install -y --no-install-recommends "$package"
        sed -i 's/^Origin: .*/Origin: Somebody else/' "$served/changed/apt/dists/$ID$VERSION_ID/InRelease"
        ;;
    fedora)
        readme_setup dnf "sudo dnf install $package"
        # Fedora's dnf skips a repository it cannot verify and exits 0: ask what is in it.
        # With -y, so a key the site offers would be taken if dnf looked there for one
        refresh() {
            [ -n "$(dnf -y repoquery --refresh --repo sendspin "$package")" ]
        }
        dnf -y install "$package-0"
        dnf -y upgrade "$package"
        echo "<!-- changed -->" >>"$served/changed/rpm/fedora$VERSION_ID/repodata/repomd.xml"
        ;;
    *) fail "no test for $ID" ;;
esac

installed=$("$package" --version)
[ "$installed" = "$version" ] || fail "version $installed after the upgrade, expected $version"

serve changed
! refresh || fail "took an index that was changed after signing"
serve other-key
! refresh || fail "took an index signed with another key"
serve good
refresh || fail "the repository no longer works"
