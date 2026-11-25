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

CI builds in Fedora 44, Debian 13 and Ubuntu 26.04 containers, runs the tests there against a
headless PipeWire, repeats them under the sanitizers on Fedora, and checks formatting. Debian 13
is the floor: do not use Qt newer than 6.8 or PipeWire newer than 1.4.2 API without a fallback.

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
- `src/cast/` — casting to speakers. `speaker_menu` is the picker's rules as plain functions, one
  test per rule; `cast_engine` carries them out on the embedded server; `cast_feed` is the only
  part on PipeWire's realtime thread. `cast_engine_test` runs a real sendspin-cpp player on the
  loopback interface, so it needs no speaker and no mDNS.
- `ci/` — `start-pipewire.sh` and the daemon config it loads: a null sink, no hardware.
- `cmake/sendspin_libs.cmake` — the pinned Sendspin libraries. Both fetch the same third-party
  dependencies; the order of declarations there is what keeps them to one copy. Fix collisions
  there, never by forking a library.
