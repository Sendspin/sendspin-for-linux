# Sendspin for Linux

Qt 6 tray app on sendspin-cpp (client) and sendspin-server-cpp (server), with PipeWire for
audio. The design is `docs/SPEC.md`.

## Build and test

```sh
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
```

CI builds in Fedora 44 and Debian 13 containers. Debian 13 is the floor: do not use Qt newer
than 6.8 or PipeWire newer than 1.4.2 API without a fallback.

## Conventions

- **KISS.** No abstraction ahead of a second user of it. No options, layers or interfaces "for
  later".
- **C++20.** Format with the repo's `.clang-format` (`clang-format -i src/*.cpp src/*.h`).
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
- `cmake/sendspin_libs.cmake` — the pinned Sendspin libraries. Both fetch the same third-party
  dependencies; the order of declarations there is what keeps them to one copy. Fix collisions
  there, never by forking a library.
