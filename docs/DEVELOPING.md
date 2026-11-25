# Developing Sendspin for Linux

A tray app built on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) and
[sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) with Qt 6 and PipeWire.
The design is in [SPEC.md](SPEC.md). Packages, releases and the package repositories are in
[MAINTAINING.md](MAINTAINING.md).

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

## Install from source

The release's `sendspin-for-linux-<version>.tar.xz` carries everything the build needs besides
Qt 6, PipeWire, Avahi and OpenSSL 3, and downloads nothing ([NOTICES.md](../NOTICES.md) lists
what is in it). With the packages under [Build](#build) installed:

```sh
cmake -B build -G Ninja -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

## Tests

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
`--verbose` it logs every 5 s how far off the group's time it plays. An output whose delay is longer than the speakers' buffer, as Bluetooth's can be, cannot
play along and stays silent.

## Logging

Info and up by default, debug with `--verbose`; the rules are in [SPEC.md](SPEC.md#logging), and
what a user does with them in the README's "Getting logs". `QT_LOGGING_RULES` works as in any Qt
app and wins over both: `QT_LOGGING_RULES="sendspin.*.debug=true"` is `--verbose`, and
`sendspin.cast.debug=true` alone turns up one part. `sendspin.server.lib` and
`sendspin.client.lib` are the two Sendspin libraries. No key, PSK or pairing code is logged at
any level.

## Files

Keys and pairing records are stored as owner-only files under `$XDG_DATA_HOME/sendspin/`
(`~/.local/share/sendspin/`). The servers that paired with this PC are listed from
`paired_servers.ini` there. The settings are `~/.config/sendspin/sendspin.conf`; the
[README](../README.md#settings) has its keys.

Open at login starts the app with `--background`: the tray alone, no picker. A package installs
the entry for every user, `/etc/xdg/autostart/com.sendspin_audio.Sendspin.desktop`, and the
switch in Settings writes `~/.config/autostart/com.sendspin_audio.Sendspin.desktop` only where
yours differs: to switch the package's off, or to switch it on where there is none. The Flatpak
asks the desktop's Background portal instead.
