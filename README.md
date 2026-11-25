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

Fedora 44, Ubuntu 26.04 and Debian 13, or newer, with PipeWire. Each
[release](https://github.com/Sendspin/sendspin-for-linux/releases) carries the files below and
their `SHA256SUMS`.

1. **Distro packages.** Download the one for your release and install it with its dependencies:

   ```sh
   sudo apt install ./sendspin-for-linux-*-debian13-amd64.deb     # Debian 13
   sudo apt install ./sendspin-for-linux-*-ubuntu26.04-amd64.deb  # Ubuntu 26.04
   sudo dnf install ./sendspin-for-linux-*-fedora44-x86_64.rpm    # Fedora 44
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

There is no package repository, AUR package or updater yet: a new release is a new download.

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
download in `third_party/fetched/`, and its commit's date in `RELEASE_DATE` for the metainfo. A
tree that has that directory builds offline. The packages are CPack's, built from the tarball
on the release they are for, and the Flatpak is `flatpak/com.sendspin_audio.Sendspin.yml`,
built inside the tarball:

```sh
scripts/vendor.sh dist
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build && (cd build && cpack -G DEB)   # or RPM
```

CI builds all of them on every change, builds the tarball with no network, and installs and
starts each package in a clean container. To release, set `project(VERSION)` in
`CMakeLists.txt` and push the tag `v<version>`: the Release workflow attaches the tarball, the
packages, the Flatpak bundle, the manifest Flathub builds from and `SHA256SUMS` to a draft
release, which a person publishes. [flatpak/FLATHUB.md](flatpak/FLATHUB.md) has the Flathub
side: submitting the app, and what each release needs there.
