# Sendspin for Linux

Qt 6 tray app on sendspin-cpp (client) and sendspin-server-cpp (server), with PipeWire for
audio. The design is `docs/SPEC.md`.

## Build and test

```sh
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
```

`ctest` skips the audio tests unless a PipeWire daemon is running. `ci/start-pipewire.sh` runs a
command against a private headless one, as CI does; it is safe on a desktop, and under it a
missing daemon or tool fails those tests instead of skipping them:

```sh
ci/start-pipewire.sh ctest --test-dir build
```

`-DSENDSPIN_SANITIZE=ON` builds the app and its tests with ASan and UBSan.

`tests/interop/run.sh` runs the built app against other implementations, under the same private
PipeWire: see `tests/interop/` below.

CI builds in Fedora 44, Debian 13 and Ubuntu 26.04 containers, runs the tests there against a
headless PipeWire, repeats them under the sanitizers on Fedora, runs the interop tests on Fedora,
and checks formatting. Debian 13 is the floor: do not use Qt newer than 6.8 or PipeWire newer
than 1.4.2 API without a fallback.

## Conventions

- **KISS.** No abstraction ahead of a second user of it. No options, layers or interfaces "for
  later".
- **C++20.** Format with the repo's `.clang-format` (`clang-format -i src/*.cpp src/*.h src/*/*.cpp src/*/*.h`).
  CI checks with clang-format 20.1.8 and other releases format differently; `pipx run
  clang-format==20.1.8` gets that one anywhere.
- **Naming** follows the Sendspin libraries: `PascalCase` types, `snake_case` functions and
  variables, `trailing_underscore_` members reached through `this->`. Qt signals, slots,
  properties and test slots use Qt's `camelCase`.
- **Comments** are terse and say why, not what.
- **Tests sit next to the code they cover**: `foo.cpp` and `foo_test.cpp` in the same
  directory, Qt Test, registered with `sendspin_add_test()` in `src/CMakeLists.txt`. Qt Test
  rather than GoogleTest because it ships with Qt and brings `QSignalSpy` and event-loop
  helpers.
- **Secrets.** Never log key material or blob contents; storage key names are fine.

## Layout

- `src/` — the app. Everything except `main.cpp` is the `sendspin_app` library the tests link.
- `src/audio/` — PipeWire. Its tests need a running daemon (and `pw-play`, `pw-dump`, `wpctl`) and
  skip without one.
- `src/audio/play_along.*` plays the group's audio on this PC's own output; `drift_corrector` is
  its timing as plain logic, and `PlayAlong::render()` runs in tests without a daemon.
- `src/cast/` — casting to speakers. `speaker_menu` is the picker's rules as plain functions, one
  test per rule; `cast_engine` carries them out on the embedded server; `cast_feed` is the only
  part on PipeWire's realtime thread. `cast_engine_test` runs a real sendspin-cpp player on the
  loopback interface, so it needs no speaker and no mDNS; `test_peer.h` is that player, for any
  test that needs a speaker.
- `src/picker/` — the picker and what the tray shows. `picker` asks `speaker_menu` what each row
  shows and what a click leads to, and carries that out on the cast engine, the client and the
  default output; it holds no rules of its own. `qml/` is the window. `picker_test` loads the QML
  offscreen and checks each status against its row.
- `data/` — the launcher's entry, the autostart entry packages install, the AppStream metainfo
  and the icons, installed by `cmake --install`. The app icon's PNGs are rendered from its SVG
  by `scripts/render-app-icon.py`.
- `src/autostart.*` — open at login: the user's entry over a package's, the Background portal
  under Flatpak. Either starts the app with `--background`. The portal's entry has the same
  name as the user's own, and is left alone.
- `src/client/` — the Sendspin client and its roles. `src/mpris.*` shows what a server plays here;
  its bus test needs a session bus and skips without one.
- `src/settings_model.*` — what the Settings window shows and changes; `src/qml/` is the window,
  one file per section, with no logic of its own. `app_test` loads it and fails on anything the
  QML engine warns of.
- `third_party/` — vendored sources, listed in its `README.md`. Not formatted, not edited.
  `third_party/fetched/` exists only in a release's source tarball (`scripts/vendor.sh`): what
  FetchContent would download, which the build then uses instead. `NOTICES.md` lists it; update
  it with every pin. `cmake --install` puts its license files with the binary.
- `cmake/package.cmake` — the .deb and .rpm, by CPack. A QML import or Qt plugin the app starts
  to use is a dependency to add there, and in `arch/PKGBUILD`: Arch's package and the AUR's
  files, whose `pkgver` follows `project(VERSION)` (`.SRCINFO` is `makepkg --printsrcinfo`).
  `flatpak/` is the Flatpak's manifest.
- `.github/workflows/packages.yml` — builds the tarball offline, the packages (Arch's by
  `makepkg`) and the Flatpak, and starts each package in a clean container (`ci/smoke-test.sh`, which runs the app with
  `--check`: both windows and an SVG, or it exits); `ci.yml` and `release.yml` both call it. The version is `project(VERSION)` alone; a tag `v<version>` drafts a release.
- `ci/` — `start-pipewire.sh` and the daemon config it loads: a null sink, no hardware.
- `tests/interop/` — the app end to end, in Python on pytest, run by `uv`: `test_cast.py` casts to
  a real sendspin-cpp-cli player, `test_music_assistant.py` is a server on aiosendspin that pairs
  with the app's client, takes its source and plays to it. Both versions are pinned there
  (`run.sh`; `requirements.in`, locked with hashes in `requirements.txt`). No mDNS and no system
  bus: each side is given the other's loopback URL. A test
  that fails for a known upstream bug is a strict `xfail` naming the issue.
- `cmake/sendspin_libs.cmake` — the pinned Sendspin libraries. Both fetch the same third-party
  dependencies; the order of declarations there is what keeps them to one copy. Fix collisions
  there, never by forking a library.
