#!/bin/sh
# Prints the manifest Flathub builds: flatpak/com.sendspin_audio.Sendspin.yml with a release's
# source tarball in place of the tree it sits in (flatpak/FLATHUB.md).
#
#     scripts/flathub-manifest.sh <tarball's URL> <its sha256>
set -eu

[ $# -eq 2 ] || { echo "usage: $0 <tarball's URL> <its sha256>" >&2; exit 2; }
manifest=$(dirname "$0")/../flatpak/com.sendspin_audio.Sendspin.yml
# The source is the manifest's last two lines; the header is about building in the tree
grep -q '^      - type: dir$' "$manifest" && [ "$(tail -n 1 "$manifest")" = '        path: ..' ] ||
    { echo "flathub-manifest: $manifest no longer ends with its dir source" >&2; exit 1; }

sed -e '1,/^$/d' -e '/^      - type: dir$/,$d' "$manifest"
cat <<SOURCE
      - type: archive
        url: $1
        sha256: $2
        # Flathub's bot opens the pull request for the next release
        x-checker-data:
          type: json
          url: https://api.github.com/repos/Sendspin/sendspin-for-linux/releases/latest
          version-query: .tag_name | sub("^v"; "")
          url-query: >-
            .assets[] | select(.name == "sendspin-for-linux-" + \$version + ".tar.xz") |
            .browser_download_url
SOURCE
