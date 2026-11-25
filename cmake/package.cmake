# The .deb and .rpm, by CPack: `cpack -G DEB` or `cpack -G RPM` in a build configured with
# -DCMAKE_INSTALL_PREFIX=/usr. Built on the release they are for, so the shared libraries they
# need (Qt 6, libpipewire, avahi-client, OpenSSL 3) are that release's, found by its own tools.

set(CPACK_PACKAGE_NAME sendspin-for-linux)
set(CPACK_PACKAGE_VENDOR "Sendspin")
set(CPACK_PACKAGE_CONTACT "Sendspin for Linux contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Play this PC's sound on Sendspin speakers")
set(CPACK_PACKAGE_DESCRIPTION
    "A tray app that plays what this PC plays on Sendspin speakers, in sync, and joins Music Assistant as a player and as an input.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Sendspin/sendspin-for-linux")
set(CPACK_STRIP_FILES ON)
# /etc/xdg/autostart is outside the prefix
set(CPACK_SET_DESTDIR ON)
# A configuration file to both: an upgrade keeps an admin's edit, a Hidden=true for one
set(SENDSPIN_AUTOSTART_ENTRY /etc/xdg/autostart/com.sendspin_audio.Sendspin.desktop)
file(WRITE ${CMAKE_BINARY_DIR}/conffiles "${SENDSPIN_AUTOSTART_ENTRY}\n")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${CMAKE_BINARY_DIR}/conffiles)
set(CPACK_RPM_USER_FILELIST "%config(noreplace) ${SENDSPIN_AUTOSTART_ENTRY}")

# What no linked library names: the QML modules the windows import, the SVG plugin that draws the
# icons, the platform plugins, and gpgv, which verifies an update. The daemons are a desktop's
# own, so only recommended, and so is pkexec: without it an update is only announced.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, qml6-module-qtquick-window, qml6-module-qtqml-workerscript, qt6-svg-plugins, qt6-qpa-plugins, qt6-wayland, gpgv")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pipewire, wireplumber, avahi-daemon, pkexec")

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtdeclarative, qt6-qtsvg, qt6-qtwayland, /usr/bin/gpgv")
set(CPACK_RPM_PACKAGE_RECOMMENDS "pipewire, wireplumber, avahi, polkit")
# Directories other packages own
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /etc/xdg /etc/xdg/autostart /usr/share/applications /usr/share/metainfo /usr/share/icons
    /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps
    /usr/share/icons/hicolor/symbolic /usr/share/icons/hicolor/symbolic/apps
    /usr/share/polkit-1 /usr/share/polkit-1/actions /usr/libexec)
foreach(_size 16 24 32 48 64 128 256 512)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        /usr/share/icons/hicolor/${_size}x${_size} /usr/share/icons/hicolor/${_size}x${_size}/apps)
endforeach()

include(CPack)
