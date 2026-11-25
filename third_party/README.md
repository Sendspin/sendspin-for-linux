# Vendored sources

Third-party code kept in this tree, built by the top-level `CMakeLists.txt`. Unmodified; to
update one, replace its files with the new release's and change its row.

| Source | Version | License | Files | Used for |
| --- | --- | --- | --- | --- |
| [QR Code generator library](https://github.com/nayuki/QR-Code-generator) (Project Nayuki), C++ | v1.8.0 (`7ad95ce`) | MIT, in each file's header | `qrcodegen/qrcodegen.cpp`, `qrcodegen/qrcodegen.hpp` | The QR code a pairing server asks for, in Settings |

The Sendspin libraries and what they fetch are not here: `cmake/sendspin_libs.cmake` pins them.
