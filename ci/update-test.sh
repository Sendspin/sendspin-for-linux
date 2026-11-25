#!/bin/sh
# A clean machine with the package installed from a downloaded file, and the signing key set up
# as the README says, takes itself to a later version: `sendspin-for-linux --update` against a
# stand-in for the release on GitHub, signed with a key made here. And leaves alone a download
# the key did not sign, one that is changed after the app verified it, and a signed package
# that is older than the installed one. As root; needs
# python3 to serve the release, gpg to sign it, and gpgv.
#
#     ci/update-test.sh <package> <later package> <version> <later version>
#
# pkexec is a stand-in too: there is no polkit in a container, and nobody to ask. Like the real
# one it passes no environment on.
set -eu

old=$1
new=$2
version=$3
later=$4
url=http://127.0.0.1:8000
package=sendspin-for-linux

fail() {
    echo "update-test: $*" >&2
    exit 1
}

work=$(mktemp -d)
mkdir -p "$work/gnupg" "$work/good" "$work/changed"
chmod 700 "$work/gnupg"
gpg_ci() {
    GNUPGHOME=$work/gnupg gpg --batch --quiet --pinentry-mode loopback --passphrase '' "$@"
}
gpg_ci --quick-generate-key "Sendspin CI" rsa4096 sign never

. /etc/os-release
case "$ID" in
    debian | ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        key=/etc/apt/keyrings/sendspin.asc
        apt-get install -y --no-install-recommends "$old"
        installed() {
            dpkg-query -W -f '${Version}' "$package"
        }
        ;;
    fedora)
        key=/etc/pki/rpm-gpg/RPM-GPG-KEY-sendspin
        dnf -y install "$old"
        installed() {
            rpm -q --qf '%{VERSION}' "$package"
        }
        ;;
    *) fail "no test for $ID" ;;
esac
gpg_ci --armor --export >"$key"

# The release: its files, their sums, the signature, and what GitHub's API says of them
name=$(basename "$new")
cp "$new" "$work/good/$name"
(cd "$work/good" && sha256sum -- * >SHA256SUMS)
gpg_ci --armor --detach-sign --output "$work/good/SHA256SUMS.asc" "$work/good/SHA256SUMS"
cat >"$work/good/latest" <<JSON
{"tag_name": "v$later", "html_url": "$url/",
 "assets": [{"name": "$name", "browser_download_url": "$url/$name"},
            {"name": "SHA256SUMS", "browser_download_url": "$url/SHA256SUMS"},
            {"name": "SHA256SUMS.asc", "browser_download_url": "$url/SHA256SUMS.asc"}]}
JSON
cp "$work"/good/* "$work/changed/"
echo "changed after signing" >>"$work/changed/$name"

# One address, and what it serves is swapped under it
ln -s good "$work/now"
python3 -m http.server --bind 127.0.0.1 --directory "$work/now" 8000 >/dev/null 2>&1 &
server=$!
trap 'kill $server; GNUPGHOME=$work/gnupg gpgconf --kill gpg-agent 2>/dev/null || true' EXIT
for _ in 1 2 3 4 5; do
    python3 -c "import urllib.request; urllib.request.urlopen('$url/latest')" 2>/dev/null && break
    sleep 1
done

pkexec=/usr/local/bin/pkexec
honest='exec env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"'
stand_in() {
    printf '#!/bin/sh\n%s\n' "$1" >"$pkexec"
    chmod +x "$pkexec"
}
update() {
    SENDSPIN_UPDATE_URL=$url/latest "$package" --update
}
unchanged() {
    [ "$(installed)" = "$version" ] || fail "version $(installed) after $1"
}

stand_in "$honest"
unchanged "installing $old"

ln -sfn changed "$work/now"
! update || fail "took a download that was changed after signing"
unchanged "a download that was changed after signing"
ln -sfn good "$work/now"

# $2 is the mode, $3 the download: the app has verified it by now
stand_in "echo swapped >\"\$3\"
$honest"
! update || fail "installed a file that was swapped after the app verified it"
unchanged "a file that was swapped after the app verified it"

stand_in "$honest"
update || fail "the update failed"
[ "$(installed)" = "$later" ] || fail "version $(installed) after the update, expected $later"
[ "$("$package" --version)" = "$later" ] || fail "the app is not the later one"
update || fail "the newest version found something to do"

# A signed release whose package is an older one: what runs as root goes by the package, not by
# what the release calls it
older=$(echo "$name" | sed "s/-$later-/-1000.0.0-/")
mkdir "$work/older"
cp "$old" "$work/older/$older"
(cd "$work/older" && sha256sum -- * >SHA256SUMS)
gpg_ci --armor --detach-sign --output "$work/older/SHA256SUMS.asc" "$work/older/SHA256SUMS"
sed -e "s/v$later/v1000.0.0/" -e "s/$name/$older/g" "$work/good/latest" >"$work/older/latest"
ln -sfn older "$work/now"
! update || fail "took an older package for an update"
[ "$(installed)" = "$later" ] || fail "version $(installed) after an older package was offered"
