# Sendspin for Linux

A [Sendspin](https://github.com/Sendspin/spec) desktop app for Linux: a tray app that plays
Music Assistant on this machine, offers its audio as a source, and casts what the desktop plays
to Sendspin speakers. It is built on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp)
and [sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) with Qt 6 and
PipeWire. The design is in [docs/SPEC.md](docs/SPEC.md).

Click the tray icon, or start Sendspin again from the launcher, to pick the speakers this PC
plays on. Music Assistant finds the PC on the network: pair it there with the code that pops up
here, and it can play to this PC and take this PC's sound as an input.

## Install

Fedora 44, Ubuntu 26.04 and Debian 13, or newer, and Arch Linux, with PipeWire. Each
[release](https://github.com/Sendspin/sendspin-for-linux/releases) carries the files below and
their `SHA256SUMS`.

1. **Distro packages**, from the package repository, so the system's updates bring new versions.

   Debian 13 and Ubuntu 26.04:

   ```sh
   sudo wget -O /etc/apt/keyrings/sendspin.asc https://sendspin.github.io/sendspin-for-linux/sendspin.asc
   . /etc/os-release && echo "deb [signed-by=/etc/apt/keyrings/sendspin.asc] https://sendspin.github.io/sendspin-for-linux/apt $ID$VERSION_ID main" | sudo tee /etc/apt/sources.list.d/sendspin.list
   sudo apt update && sudo apt install sendspin-for-linux
   ```

   On a distribution built on one of them, write `debian13` or `ubuntu26.04` in place of
   `$ID$VERSION_ID`.

   Fedora 44:

   ```sh
   sudo curl -fsSLo /etc/pki/rpm-gpg/RPM-GPG-KEY-sendspin https://sendspin.github.io/sendspin-for-linux/sendspin.asc
   sudo curl -fsSLo /etc/yum.repos.d/sendspin.repo https://sendspin.github.io/sendspin-for-linux/rpm/sendspin.repo
   sudo dnf install sendspin-for-linux
   ```

   The repositories are signed with the key the first line downloads, and from then on the
   system takes nothing that key did not sign. Before installing, `gpg --show-keys` on that file
   prints the key's fingerprint, which should be:

   ```
   (not created yet: the fingerprint goes here with the first release)
   ```

   Or, and on Arch for now,
   download a release's package and install it with its dependencies. Nothing but Sendspin
   itself updates it then (see [Updates](#updates)):

   ```sh
   sudo apt install ./sendspin-for-linux-*-debian13-amd64.deb     # Debian 13
   sudo apt install ./sendspin-for-linux-*-ubuntu26.04-amd64.deb  # Ubuntu 26.04
   sudo dnf install ./sendspin-for-linux-*-fedora44-x86_64.rpm    # Fedora 44
   sudo pacman -U ./sendspin-for-linux-*-archlinux-x86_64.pkg.tar.zst  # Arch Linux
   ```

   Sendspin then starts with your session, in the tray. **Open at login** in Settings switches
   that off for you.

2. **Flatpak.** Not on Flathub yet; the release's bundle installs with its runtime from Flathub:

   ```sh
   flatpak install --user ./sendspin-for-linux-*-x86_64.flatpak
   ```

   Here **Open at login** is off until you switch it on.

3. **From source.** The release's `sendspin-for-linux-<version>.tar.xz` carries everything the
   build needs besides Qt 6, PipeWire, Avahi and OpenSSL 3, and downloads nothing
   ([NOTICES.md](NOTICES.md) lists what is in it). With the packages under
   [Build](#build) installed:

   ```sh
   cmake -B build -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
   cmake --build build
   sudo cmake --install build
   ```

The Arch package is not in the AUR or a repository yet: there, a new release is a new download.

## Updates

Once a day Sendspin asks GitHub whether there is a newer release, and says so in Settings and in
the tray menu. **Check for updates** in Settings turns that off. It sends nothing but the
request.

- Installed from the package repository, or as the Flatpak: the system's updates bring the new
  version, and Sendspin only tells you of it.
- Installed from a downloaded `.deb` or `.rpm`: **Install update** in Settings downloads the new
  package, verifies it, asks for an administrator's password and installs it, then restarts
  Sendspin. `sendspin-for-linux --update` does the same from a terminal.

  It verifies the download against the signing key on your machine, and never fetches one. So
  the button is there once the key is: the first line of the repository setup above, for your
  distro, with the fingerprint checked as it says. Without the key, Sendspin opens the release's
  page instead.

## Settings and files

Keys and pairing records are stored as owner-only files under `$XDG_DATA_HOME/sendspin/`
(`~/.local/share/sendspin/`).

**Settings…** in the tray menu, or the picker's gear, opens the Settings window. Every change
applies at once, except the device name, which servers see from the next start. The settings are
kept in `~/.config/sendspin/sendspin.conf`:

```ini
[client]
; what servers call this PC, instead of its hostname
name=Den PC
; false makes a server pair before it may play here
unpaired_access=true

[player]
; the PipeWire node.name to play on, instead of the default output
output=alsa_output.usb-Example_DAC-00.analog-stereo

[mpris]
; false keeps what a server plays here out of the desktop's media controls
enabled=true

[updates]
; false never asks GitHub for a newer release
check=true
```

Open at login starts the app with `--background`: the tray alone, no picker. A package installs
the entry for every user, `/etc/xdg/autostart/com.sendspin_audio.Sendspin.desktop`, and the
switch in Settings writes `~/.config/autostart/com.sendspin_audio.Sendspin.desktop` only where
yours differs: to switch the package's off, or to switch it on where there is none. The Flatpak
asks the desktop's Background portal instead. The servers that paired with this PC are listed
from `~/.local/share/sendspin/paired_servers.ini`.

# For developers

## Build

Needs CMake 3.28, a C++20 compiler, Qt 6.8, PipeWire 1.4.2, avahi-client and OpenSSL 3, or
newer. Debian 13 is the oldest supported release.

Fedora:

```sh
sudo dnf install cmake ninja-build gcc-c++ git patch pkgconf-pkg-config xz \
    qt6-qtbase-devel qt6-qtdeclarative-devel pipewire-devel avahi-devel openssl-devel
```

Debian and Ubuntu:

```sh
sudo apt install cmake ninja-build g++ git patch pkg-config xz-utils \
    qt6-base-dev qt6-declarative-dev libpipewire-0.3-dev libavahi-client-dev libssl-dev
```

Then:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/src/sendspin-for-linux
```

The first configure downloads the Sendspin libraries and their dependencies.

The audio tests need a PipeWire daemon and skip without one. To run them anywhere, a container
included, against a private headless daemon (needs `dbus-daemon`, `pipewire`, `wireplumber` and
the `pw-play`, `pw-dump` and `wpctl` tools):

```sh
ci/start-pipewire.sh ctest --test-dir build
```

The interop tests run the built app against other implementations, on loopback under the same
private daemon: casting to a [sendspin-cpp-cli](https://github.com/Sendspin/sendspin-cpp-cli)
player, and as Music Assistant sees it, with a server scripted on
[aiosendspin](https://github.com/Sendspin/aiosendspin) that pairs by the code, takes the PC as a
source and plays to it. They need `uv` and `curl` as well, and the libraries the player release
links (Fedora: `portaudio` and `avahi-compat-libdns_sd`):

```sh
tests/interop/run.sh
```

The player is downloaded once, an x86_64 release pinned in `run.sh`; `SENDSPIN_CLI` names another.
Two of the cast tests are expected to fail until the silent-speaker problem below is fixed
upstream, and fail the run when they start passing.

## Casting without the UI

The speakers can be chosen from the command line too. Each of these runs the app without its
tray icon until interrupted, and casts what plays to the Sendspin output:

```sh
./build/src/sendspin-for-linux --list              # the speakers found, as they change
./build/src/sendspin-for-linux --add "Kitchen"     # add a speaker to the group
./build/src/sendspin-for-linux --remove "Kitchen"  # take it out
./build/src/sendspin-for-linux --add "Kitchen" --play-along  # and play on this PC too, in sync
```

The speakers added are remembered in `~/.local/share/sendspin/cast.ini`, and the app casts to
them when it starts normally too. Adding a speaker that another server is playing to takes it
from that server.

With this PC's output ticked in the picker beside a speaker, the app plays along on it: the
system's default output, or the one that was the default before the Sendspin output. With
`QT_LOGGING_RULES=sendspin.cast.info=true` it logs every 5 s how far off the group's time it
plays. An output whose delay is longer than the speakers' buffer, as Bluetooth's can be, cannot
play along and stays silent.

A speaker built on sendspin-cpp v0.10.0 or later stays silent for now: sendspin-server-cpp
starts its stream before it declares playback, which those clients ignore. See "Depends on" in
[docs/SPEC.md](docs/SPEC.md).

## Packages and releases

`scripts/vendor.sh` makes the source tarball: the tree at `HEAD` with what the build would
download in `third_party/fetched/`. A tree that has that directory builds offline. The packages
are CPack's, built from the tarball on the release they are for, Arch's is `makepkg`'s from
`arch/PKGBUILD`, and the Flatpak is `flatpak/com.sendspin_audio.Sendspin.yml`, built inside the
tarball:

```sh
scripts/vendor.sh dist
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build && (cd build && cpack -G DEB)   # or RPM
```

CI builds all of them on every change, builds the tarball with no network, and installs and
starts each package in a clean container. To release, set `project(VERSION)` in
`CMakeLists.txt` and push the tag `v<version>`: the Release workflow attaches the tarball, the
packages, the Flatpak bundle and `SHA256SUMS` to a draft release. Its last job signs
`SHA256SUMS` with the repositories' key, once the `package-signing` environment's reviewer
approves it: that signature is what an installed app verifies an update with, so publish the
draft after it is there. CI has each package update itself to a later build in a clean container
(`ci/update-test.sh`).

Publishing the release runs the Package repositories workflow: `scripts/package-repos.sh` builds
apt and dnf repositories from the packages of every published release, signs them, and the
workflow puts them on GitHub Pages. It can be run by hand too, after a release is deleted or the
key is replaced. CI builds the same repositories with a key of its own on every change, and
installs and upgrades from them in clean containers (`ci/repo-fixture.sh`, `ci/repo-test.sh`).

The script takes any package named `<name>-<version>-<distro>-<arch>.deb` or `.rpm`, so another
project's packages are published by downloading them into the same directory.

Only what the Release workflow built is signed: each package has to match its release's
`SHA256SUMS` and carry that workflow's build attestation, or the run fails.

Once, by a maintainer:

1. Create the signing key on a machine you trust: a primary key that only certifies and stays
   there, and a signing subkey, which alone goes to GitHub. Give it a passphrase.

   ```sh
   gpg --quick-generate-key "Sendspin packages" rsa4096 cert never
   gpg --quick-add-key <primary fingerprint> rsa4096 sign never
   gpg --list-keys --with-subkey-fingerprints "Sendspin packages"
   gpg --armor --export-secret-subkeys '<subkey fingerprint>!'
   ```

   Put the primary key's fingerprint in this README's install section. Keep the primary key and
   the passphrase somewhere safe: machines that installed from the repositories trust this key,
   and a new one means setting each of them up again.
2. Create the environment `package-signing`, with a required reviewer and its deployment
   branches and tags limited to `main` and `v*`. Add the export there as the secret
   `REPO_GPG_PRIVATE_KEY` and the passphrase as `REPO_GPG_PASSPHRASE`: environment secrets, not
   repository secrets, so only the jobs that name the environment can read them: this one, and
   the Release workflow's, which signs a draft's `SHA256SUMS`. Each then waits for the reviewer.
3. Set the Pages source to **GitHub Actions**. Publishing a release runs on its tag, and the
   `github-pages` environment allows only the default branch at first: add `v*` to its
   deployment branches and tags.
4. Add a ruleset that restricts who can create `v*` tags, and turn on immutable releases, so a
   published release's files cannot be replaced before they are signed.

`arch/PKGBUILD` is the AUR's: it downloads a published release's tarball and checks its sha256,
which only exists once the release does. So set `pkgver` with `project(VERSION)`, and after
publishing, on Arch (`updpkgsums` is in `pacman-contrib`):

```sh
cd arch && updpkgsums && makepkg --printsrcinfo >.SRCINFO
```

Commit both, and copy them to the AUR repository, which needs a maintainer's account. CI builds
the PKGBUILD against the tarball it has just made instead, with that tarball's sum.
