# Sendspin for Linux

A [Sendspin](https://github.com/Sendspin/spec) desktop app for Linux: a tray app that plays
Music Assistant on this machine, offers its audio as a source, and casts what the desktop plays
to Sendspin speakers. It is built on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp)
and [sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) with Qt 6 and
PipeWire. The design is in [docs/SPEC.md](docs/SPEC.md).

So far: the "Sendspin" output device, and the PC as an input source for Music Assistant. MA
finds the PC on the network; pair it there with the code that pops up here, then pick the PC as
an input. Casting to speakers, playing MA on this PC and the picker are still to come.

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

Keys and pairing records are stored as owner-only files under `$XDG_DATA_HOME/sendspin/`
(`~/.local/share/sendspin/`).

Until there is a Settings window, two settings are read at startup from
`~/.config/sendspin/sendspin.conf`:

```ini
[player]
; the PipeWire node.name to play on, instead of the default output
output=alsa_output.usb-Example_DAC-00.analog-stereo

[mpris]
; false keeps what a server plays here out of the desktop's media controls
enabled=true
```
