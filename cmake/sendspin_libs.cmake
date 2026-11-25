# sendspin-server-cpp (target sendspin_server_cpp) and sendspin-cpp (target sendspin).
#
# Both fetch ArduinoJson, noise-c, micro-flac and micro-opus under the same FetchContent names,
# and FetchContent keeps the first declaration of a name. Two things make them share one copy:
#
#   - The server comes first. Its noise_c target carries AES-GCM (OpenSSL), which the server
#     requires; the client reuses an existing noise_c target but would build one without AES.
#   - micro-flac is declared here, ahead of both, at the newer of their two pins (server v0.3.0,
#     client v0.3.1).
#
# ArduinoJson (v7.4.1), noise-c (v0.1.13) and micro-opus (v0.3.5) are pinned alike upstream.
# Re-check all four when bumping either library.

include(FetchContent)

# A release tarball carries what would be fetched, here and by the two libraries, in
# third_party/fetched/ under FetchContent's own names (scripts/vendor.sh). With it nothing is
# downloaded: distro and Flatpak builds have no network.
file(GLOB _fetched ${CMAKE_SOURCE_DIR}/third_party/fetched/*)
if(_fetched)
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
    # As the server does when it fetches: a system Boost from its pin on is used instead
    find_package(Boost 1.90.0 CONFIG QUIET)
    foreach(_dir ${_fetched})
        get_filename_component(_name ${_dir} NAME)
        if(NOT (_name STREQUAL "boost" AND Boost_FOUND))
            string(TOUPPER ${_name} _name)
            set(FETCHCONTENT_SOURCE_DIR_${_name} ${_dir} CACHE PATH "" FORCE)
        endif()
    endforeach()
endif()

FetchContent_Declare(
    micro_flac
    GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
    GIT_TAG        d0cadb9ded5779d2de8f957530c77845c42770bd  # v0.3.1
    GIT_SUBMODULES "lib/micro-ogg-demuxer"
    SYSTEM
    EXCLUDE_FROM_ALL
)

# No release yet, so a main commit
FetchContent_Declare(
    sendspin_server_cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-server-cpp.git
    GIT_TAG        f217a96d4b475bd06157e75ad4ee035db78b55c1  # main, 2026-11-20
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(
    sendspin_cpp
    GIT_REPOSITORY https://github.com/Sendspin/sendspin-cpp.git
    GIT_TAG        a10289a838a0c3e777efdd73b8ad19506b8f2bc9  # v0.10.0
    GIT_SUBMODULES ""
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(micro_flac sendspin_server_cpp sendspin_cpp)
