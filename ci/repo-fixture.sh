#!/bin/sh
# What ci/repo-test.sh installs from: the repositories of scripts/package-repos.sh, built from
# this run's packages and an empty version 0 of each to upgrade from, signed with a key made
# here. And the same again under a second key, which a machine set up for the first refuses.
#
#     ci/repo-fixture.sh <packages dir> <out dir> <base URL>
#
# Leaves <out dir>/site and <out dir>/site-other-key.
set -eu

packages=$1
out=$2
url=$3
here=$(dirname "$0")

work=$(mktemp -d)
trap 'GNUPGHOME=$work/gnupg gpgconf --kill gpg-agent 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir -p "$work/packages" "$work/gnupg" "$out"
chmod 700 "$work/gnupg"
cp "$packages"/*.deb "$packages"/*.rpm "$work/packages/"

for deb in "$packages"/*.deb; do
    old=$(basename "$deb" | sed 's/^\(.*\)-[^-]*-\([^-]*-[^-]*\.deb\)$/\1-0-\2/')
    mkdir -p "$work/deb/DEBIAN"
    cat >"$work/deb/DEBIAN/control" <<EOF
Package: $(dpkg-deb -f "$deb" Package)
Version: 0
Architecture: $(dpkg-deb -f "$deb" Architecture)
Maintainer: CI <ci@invalid>
Description: an empty package to upgrade from
EOF
    dpkg-deb --root-owner-group --build "$work/deb" "$work/packages/$old" >/dev/null
done

for rpm in "$packages"/*.rpm; do
    old=$(basename "$rpm" | sed 's/^\(.*\)-[^-]*-\([^-]*-[^-]*\.rpm\)$/\1-0-\2/')
    arch=$(rpm -qp --qf '%{ARCH}' "$rpm" 2>/dev/null)
    cat >"$work/old.spec" <<EOF
Name: $(rpm -qp --qf '%{NAME}' "$rpm" 2>/dev/null)
Version: 0
Release: 1
Summary: An empty package to upgrade from
License: Apache-2.0
%description
An empty package to upgrade from.
%files
EOF
    rpmbuild --quiet -bb --target "$arch" --define "_topdir $work/rpmbuild" "$work/old.spec" >/dev/null
    mv "$work"/rpmbuild/RPMS/*/*.rpm "$work/packages/$old"
done

# As docs/MAINTAINING.md has the release's key: a passphrase, and only the signing subkey
# leaves the machine that made it
passphrase=throwaway
gpg_ci() {
    GNUPGHOME=$work/gnupg gpg --batch --quiet --pinentry-mode loopback --passphrase "$passphrase" "$@"
}
for site in site site-other-key; do
    rm -rf "$work/gnupg"/*
    gpg_ci --quick-generate-key "Sendspin CI ($site)" rsa4096 cert never
    primary=$(gpg_ci --list-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')
    gpg_ci --quick-add-key "$primary" rsa4096 sign never
    subkey=$(gpg_ci --list-keys --with-colons | awk -F: '$1 == "fpr" { fpr = $10 } END { print fpr }')
    key=$(gpg_ci --armor --export-secret-subkeys "$subkey!")
    GNUPGHOME=$work/gnupg gpgconf --kill gpg-agent
    REPO_GPG_PRIVATE_KEY=$key REPO_GPG_PASSPHRASE=$passphrase \
        "$here/../scripts/package-repos.sh" "$work/packages" "$out/$site" "$url"
done
