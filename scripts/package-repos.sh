#!/bin/sh
# Signed apt and dnf repositories, as a directory to serve at <base URL>, from release packages
# named <name>-<version>-<distro>-<arch>.deb or .rpm. <distro> is ID and VERSION_ID of the
# release's /etc/os-release (debian13, ubuntu26.04, fedora44): an apt suite, or a dnf
# repository at rpm/fedora$releasever. Any package named that way goes in; all versions stay.
#
#     REPO_GPG_PRIVATE_KEY=... REPO_GPG_PASSPHRASE=... \
#         scripts/package-repos.sh <packages dir> <site dir> <base URL>
#
# The key is an armored private key; the passphrase is empty for a key without one. Needs gpg,
# dpkg-deb, apt-ftparchive, rpmsign and createrepo_c.
set -eu

[ $# -eq 3 ] || { echo "usage: $0 <packages dir> <site dir> <base URL>" >&2; exit 2; }
packages=$1
site=$2
url=${3%/}

[ -n "${REPO_GPG_PRIVATE_KEY:-}" ] || { echo "package-repos: REPO_GPG_PRIVATE_KEY is not set" >&2; exit 1; }
found=$(find "$packages" -maxdepth 1 \( -name '*.deb' -o -name '*.rpm' \) | head -n 1)
[ -n "$found" ] || { echo "package-repos: no packages in $packages" >&2; exit 1; }
[ ! -e "$site" ] || { echo "package-repos: $site exists" >&2; exit 1; }

# The key lives only here, and goes with it
GNUPGHOME=$(mktemp -d)
export GNUPGHOME
trap 'gpgconf --kill gpg-agent 2>/dev/null || true; rm -rf "$GNUPGHOME"' EXIT
printf '%s' "${REPO_GPG_PASSPHRASE:-}" >"$GNUPGHOME/passphrase"
printf '%s\n' "$REPO_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
key=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')
[ -n "$key" ] || { echo "package-repos: REPO_GPG_PRIVATE_KEY holds no private key" >&2; exit 1; }

sign_args="--batch --pinentry-mode loopback --passphrase-file $GNUPGHOME/passphrase"
sign() {
    # shellcheck disable=SC2086
    gpg $sign_args --yes --local-user "$key" "$@"
}

# <distro> of a release file's name
distro() {
    name=${1##*/}
    name=${name%.*}
    name=${name%-*}
    echo "${name##*-}"
}

mkdir -p "$site"
site=$(cd "$site" && pwd)
gpg --batch --armor --export "$key" >"$site/sendspin.asc"

# apt: the two releases' packages share a name and a version, so each has its own pool
for deb in "$packages"/*.deb; do
    [ -e "$deb" ] || continue
    pool=$site/apt/pool/$(distro "$deb")
    mkdir -p "$pool"
    # apt-ftparchive tells architectures apart by this form of the name
    cp "$deb" "$pool/$(dpkg-deb -W --showformat '${Package}_${Version}_${Architecture}' "$deb").deb"
done
for pool in "$site"/apt/pool/*/; do
    [ -d "$pool" ] || continue
    suite=$(basename "$pool")
    archs=$(find "$pool" -name '*.deb' | sed 's/.*_\([^_]*\)\.deb$/\1/' | sort -u | tr '\n' ' ')
    (
        cd "$site/apt"
        for arch in $archs; do
            dir=dists/$suite/main/binary-$arch
            mkdir -p "$dir"
            apt-ftparchive --arch "$arch" packages "pool/$suite" >"$dir/Packages"
            gzip -9nk "$dir/Packages"
        done
        # Written outside the suite first, or it would list itself
        apt-ftparchive \
            -o APT::FTPArchive::Release::Origin=Sendspin \
            -o APT::FTPArchive::Release::Label=Sendspin \
            -o APT::FTPArchive::Release::Suite="$suite" \
            -o APT::FTPArchive::Release::Codename="$suite" \
            -o APT::FTPArchive::Release::Components=main \
            -o APT::FTPArchive::Release::Architectures="${archs% }" \
            release "dists/$suite" >Release
        mv Release "dists/$suite/Release"
        sign --clearsign --output "dists/$suite/InRelease" "dists/$suite/Release"
        sign --armor --detach-sign --output "dists/$suite/Release.gpg" "dists/$suite/Release"
    )
done

# dnf: the packages and the index are both signed
for rpm in "$packages"/*.rpm; do
    [ -e "$rpm" ] || continue
    dir=$site/rpm/$(distro "$rpm")
    mkdir -p "$dir"
    cp "$rpm" "$dir/"
done
for dir in "$site"/rpm/*/; do
    [ -d "$dir" ] || continue
    rpmsign --define "__gpg $(command -v gpg)" --define "_gpg_name $key" \
        --define "_gpg_sign_cmd_extra_args $sign_args" \
        --addsign "$dir"*.rpm >/dev/null
    createrepo_c --quiet "$dir"
    sign --armor --detach-sign --output "$dir/repodata/repomd.xml.asc" "$dir/repodata/repomd.xml"
done
if [ -d "$site/rpm" ]; then
    # $releasever is dnf's to fill in
    cat >"$site/rpm/sendspin.repo" <<EOF
[sendspin]
name=Sendspin
baseurl=$url/rpm/fedora\$releasever
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$url/sendspin.asc
EOF
fi
