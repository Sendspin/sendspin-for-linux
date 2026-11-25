#!/bin/sh
# A clean machine set up as the README says installs the package from the repositories of
# ci/repo-fixture.sh and upgrades it, and refuses an index that was changed or signed with
# another key. As root; needs python3 to serve them, and wget for apt.
#
#     ci/repo-test.sh <site> <site signed with another key> <version>
set -eu

version=$3
url=http://127.0.0.1:8000
package=sendspin-for-linux

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

. /etc/os-release
case "$ID" in
    debian | ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        for _ in 1 2 3 4 5; do
            wget -qO /etc/apt/keyrings/sendspin.asc "$url/sendspin.asc" && break
            sleep 1
        done
        echo "deb [signed-by=/etc/apt/keyrings/sendspin.asc] $url/apt $ID$VERSION_ID main" \
            >/etc/apt/sources.list.d/sendspin.list
        # A signature that fails is only a warning to apt-get update, without this
        refresh() {
            apt-get update -o APT::Update::Error-Mode=any
        }
        refresh
        apt-get install -y --no-install-recommends "$package=0"
        apt-get upgrade -y --with-new-pkgs --no-install-recommends
        sed -i 's/^Origin: .*/Origin: Somebody else/' "$served/changed/apt/dists/$ID$VERSION_ID/InRelease"
        ;;
    fedora)
        for _ in 1 2 3 4 5; do
            curl -fsSLo /etc/yum.repos.d/sendspin.repo "$url/rpm/sendspin.repo" && break
            sleep 1
        done
        # Fedora's dnf skips a repository it cannot verify and exits 0: ask what is in it.
        # No -y: a key other than the one it trusts is a question, and no is the answer
        refresh() {
            [ -n "$(dnf repoquery --refresh --repo sendspin "$package" </dev/null)" ]
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
