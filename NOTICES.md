# Third-party notices

Sendspin for Linux is licensed under the Apache License 2.0 (`LICENSE`). It includes, or is built
with, the third-party software below. Its binary carries all of it but what is listed under
"From the system", so `cmake --install` puts this file, `LICENSE` and each license and notice
file of the fetched sources in the documentation directory: `/usr/share/doc/sendspin-for-linux`
in the packages, `/app/share/licenses/com.sendspin_audio.Sendspin` in the Flatpak.

## In the repository

| Source | Version | License | Where |
| --- | --- | --- | --- |
| [QR Code generator library](https://github.com/nayuki/QR-Code-generator) (Project Nayuki) | v1.8.0 (`720f62b`) | MIT, in each file's header | `third_party/qrcodegen/` |
| [sendspin-cpp-cli](https://github.com/Sendspin/sendspin-cpp-cli)'s PipeWire sink and source, adapted | the commit each file names | Apache-2.0 | `src/audio/` |
| Sendspin's mark, from [sendspin-mac](https://github.com/Sendspin/sendspin-mac) (`App/sendspin-logo.svg` and its menu bar icons) | `32444d6` | Apache-2.0 | `data/icons/` |

The QR Code generator library has its license in its sources alone, so here it is:

> Copyright (c) Project Nayuki. (MIT License)
> https://www.nayuki.io/page/qr-code-generator-library
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of
> this software and associated documentation files (the "Software"), to deal in
> the Software without restriction, including without limitation the rights to
> use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
> the Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
>
> - The above copyright notice and this permission notice shall be included in
>   all copies or substantial portions of the Software.
> - The Software is provided "as is", without warranty of any kind, express or
>   implied, including but not limited to the warranties of merchantability,
>   fitness for a particular purpose and noninfringement. In no event shall the
>   authors or copyright holders be liable for any claim, damages or other
>   liability, whether in an action of contract, tort or otherwise, arising from,
>   out of or in connection with the Software or the use or other dealings in the
>   Software.

## In a release's source tarball

The build fetches these with CMake FetchContent, at the versions `cmake/sendspin_libs.cmake` and
the two Sendspin libraries pin. A release's source tarball carries them instead, unmodified
except for Boost, which is cut down to the libraries Beast's build uses, and micro-flac, which
loses its example audio (`scripts/vendor.sh`).
Each keeps its own license file.

| Source | Version | License | In the tarball |
| --- | --- | --- | --- |
| [sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) | `f217a96` (main, 2026-11-20) | Apache-2.0 | `third_party/fetched/sendspin_server_cpp` |
| [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) | v0.10.0 (`a10289a`) | Apache-2.0 | `third_party/fetched/sendspin_cpp` |
| [Boost](https://github.com/boostorg/boost): Beast, Asio and what they depend on | 1.90.0 | BSL-1.0 | `third_party/fetched/boost` |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | v7.4.1 | MIT | `third_party/fetched/arduinojson` |
| [noise-c](https://github.com/esphome-libs/noise-c) | v0.1.13 | MIT | `third_party/fetched/noise_c` |
| [micro-flac](https://github.com/esphome-libs/micro-flac) | v0.3.1 | Apache-2.0 | `third_party/fetched/micro_flac` |
| [micro-opus](https://github.com/esphome-libs/micro-opus) | v0.3.5 | Apache-2.0 | `third_party/fetched/micro_opus` |
| [libopus](https://github.com/xiph/opus), in micro-opus | v1.6.1 | BSD-3-Clause | `third_party/fetched/micro_opus/lib/opus` |
| [micro-ogg-demuxer](https://github.com/esphome-libs/micro-ogg-demuxer), in micro-flac and micro-opus | v1.2.1 and v1.2.0 | Apache-2.0 | `lib/micro-ogg-demuxer` in each |
| [pffft](https://github.com/marton78/pffft) | `aa16fd3` | BSD-style (FFTPACK license) | `third_party/fetched/pffft` |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | v12.0.1 | BSD-3-Clause | `third_party/fetched/ixwebsocket` |

sendspin-server-cpp vendors more itself, micro-resampler and stb among it: its own `NOTICES.md`
lists them.

A system Boost 1.90 or later is used instead of the vendored one where there is one.

## From the system

Qt 6 (LGPL-3.0), PipeWire (MIT), Avahi (LGPL-2.1) and OpenSSL 3 (Apache-2.0) always come from
the distribution or the Flatpak runtime, and are linked dynamically.
