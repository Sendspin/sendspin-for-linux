# 3.28: FetchContent EXCLUDE_FROM_ALL
cmake_minimum_required(VERSION 3.28)

project(sendspin_for_linux VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
# No modules here; skips a dependency scan of every source
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Floors are Debian 13's versions. Qml and Quick are not linked yet; requiring them here
# keeps every build environment ready for the Settings window.
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Qml Quick DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.4.2)
# The client's own mDNS advertisement; the server library brings its own avahi use
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)

include(cmake/sendspin_libs.cmake)

option(SENDSPIN_SANITIZE "Build the app and its tests with ASan and UBSan" OFF)

include(CTest)
add_subdirectory(src)
