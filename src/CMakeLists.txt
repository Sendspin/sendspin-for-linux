# Everything but main(), so the tests link it
add_library(sendspin_app STATIC
    app.cpp
    audio/drift_corrector.cpp
    audio/outputs.cpp
    audio/pcm_ring.cpp
    audio/play_along.cpp
    audio/playback_stream.cpp
    audio/player_formats.cpp
    audio/sendspin_output.cpp
    audio/volume_curve.cpp
    autostart.cpp
    blob_store.cpp
    cast/cast_engine.cpp
    cast/cast_feed.cpp
    cast/speaker_menu.cpp
    cast/speaker_model.cpp
    client/client.cpp
    client/mdns_advertiser.cpp
    client/now_playing.cpp
    client/player.cpp
    client/source_feed.cpp
    device_name.cpp
    discord_ipc.cpp
    discord_presence.cpp
    headless.cpp
    logging.cpp
    mpris.cpp
    notifier.cpp
    paired_servers.cpp
    persistence.cpp
    picker/picker.cpp
    picker/picker_window.cpp
    settings_model.cpp
    single_instance.cpp
    update_checker.cpp
    update_installer.cpp
)
# Plain resources rather than a QML module: a module in a static library needs its plugin
# imported by everything that links it
qt_add_resources(sendspin_app picker_qml PREFIX /qml BASE picker/qml FILES
    picker/qml/OutputRow.qml
    picker/qml/Picker.qml
    picker/qml/ServerRow.qml
)
set(SENDSPIN_ICONS ${CMAKE_SOURCE_DIR}/data/icons)
qt_add_resources(sendspin_app icons PREFIX /icons BASE ${SENDSPIN_ICONS} FILES
    ${SENDSPIN_ICONS}/computer.svg
    ${SENDSPIN_ICONS}/sendspin-symbolic.svg
    ${SENDSPIN_ICONS}/sendspin-waves-symbolic.svg
    ${SENDSPIN_ICONS}/server.svg
    ${SENDSPIN_ICONS}/settings.svg
    ${SENDSPIN_ICONS}/speaker-muted.svg
    ${SENDSPIN_ICONS}/speaker.svg
)
# The Settings window, in the same way
qt_add_resources(sendspin_app settings_qml PREFIX /
    FILES qml/Settings.qml qml/ThisPc.qml qml/Speakers.qml qml/MusicAssistant.qml)
target_include_directories(sendspin_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sendspin_app PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sendspin_app PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    sendspin_server_cpp
    sendspin
    PkgConfig::PIPEWIRE
    PkgConfig::AVAHI
)
target_link_libraries(sendspin_app PRIVATE qrcodegen)
include(GNUInstallDirs)
# What pkexec runs to install an update, and the app before it to verify one
set(SENDSPIN_UPDATE_HELPER ${CMAKE_INSTALL_FULL_LIBEXECDIR}/sendspin-for-linux/install-update)
target_compile_definitions(sendspin_app PRIVATE SENDSPIN_UPDATE_HELPER="${SENDSPIN_UPDATE_HELPER}")
target_compile_definitions(sendspin_app
    PRIVATE SENDSPIN_DISCORD_CLIENT_ID="${SENDSPIN_DISCORD_CLIENT_ID}")
# PUBLIC reaches the app and the tests, and nothing third-party
if(SENDSPIN_SANITIZE)
    target_compile_options(sendspin_app PUBLIC
        -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(sendspin_app PUBLIC -fsanitize=address,undefined)
endif()

add_executable(sendspin-for-linux main.cpp)
target_compile_options(sendspin-for-linux PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sendspin-for-linux PRIVATE SENDSPIN_VERSION="${PROJECT_VERSION}")
target_link_libraries(sendspin-for-linux PRIVATE sendspin_app)

install(TARGETS sendspin-for-linux)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/data/install-update
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/sendspin-for-linux)
# The key it verifies with: the package repositories' public key. install-update names this path
install(FILES ${CMAKE_SOURCE_DIR}/data/sendspin.asc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sendspin-for-linux)
# The words of pkexec's prompt for it
configure_file(${CMAKE_SOURCE_DIR}/data/com.sendspin_audio.Sendspin.policy.in
    com.sendspin_audio.Sendspin.policy @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.sendspin_audio.Sendspin.policy
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
install(FILES ${CMAKE_SOURCE_DIR}/data/com.sendspin_audio.Sendspin.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# For every user, until one switches it off in Settings: an entry of their own by the same name
install(FILES ${CMAKE_SOURCE_DIR}/data/autostart.desktop
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart
    RENAME com.sendspin_audio.Sendspin.desktop)
# The app's icon, and the tray's where the panel finds and colours them
install(DIRECTORY ${SENDSPIN_ICONS}/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
install(FILES ${SENDSPIN_ICONS}/sendspin-symbolic.svg ${SENDSPIN_ICONS}/sendspin-waves-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps)
# A release tarball says when its commit was made (scripts/vendor.sh): Flathub builds one again
# months later. Any other build is today's.
if(EXISTS ${CMAKE_SOURCE_DIR}/RELEASE_DATE)
    file(STRINGS ${CMAKE_SOURCE_DIR}/RELEASE_DATE SENDSPIN_RELEASE_DATE LIMIT_COUNT 1)
else()
    string(TIMESTAMP SENDSPIN_RELEASE_DATE "%Y-%m-%d" UTC)
endif()
configure_file(${CMAKE_SOURCE_DIR}/data/com.sendspin_audio.Sendspin.metainfo.xml.in
    com.sendspin_audio.Sendspin.metainfo.xml @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/com.sendspin_audio.Sendspin.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The binary carries what was fetched, and each license asks to go where it goes. Their sources
# are a release tarball's, or this build's own downloads.
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
file(GLOB _fetched ${CMAKE_SOURCE_DIR}/third_party/fetched/* ${FETCHCONTENT_BASE_DIR}/*-src)
foreach(_source ${_fetched})
    get_filename_component(_name ${_source} NAME)
    string(REGEX REPLACE "-src$" "" _name ${_name})
    set(_patterns)
    # Their own, and those of what they vendor in turn: pffft's kissfft, lib/opus,
    # third_party/micro-resampler. No deeper: every Boost library has a copy of Boost's.
    foreach(_depth "" "*/" "lib/*/" "third_party/*/")
        list(APPEND _patterns ${_source}/${_depth}LICENSE* ${_source}/${_depth}COPYING*
            ${_source}/${_depth}NOTICE*)
    endforeach()
    file(GLOB _notices LIST_DIRECTORIES false RELATIVE ${_source} ${_patterns})
    foreach(_notice ${_notices})
        get_filename_component(_dir ${_notice} DIRECTORY)
        install(FILES ${_source}/${_notice}
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/${_name}/${_dir})
    endforeach()
endforeach()

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)

    # SOURCE is the test's path without .cpp; the target takes its file name
    function(sendspin_add_test SOURCE)
        get_filename_component(NAME ${SOURCE} NAME)
        add_executable(${NAME} ${SOURCE}.cpp)
        target_compile_options(${NAME} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${NAME} PRIVATE sendspin_app Qt6::Test)
        target_compile_definitions(${NAME} PRIVATE SENDSPIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_test(NAME ${NAME} COMMAND ${NAME})
        # No display or GPU in CI, and never the settings of whoever runs the tests, nor an
        # installed package's autostart entry
        set_tests_properties(${NAME} PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config;XDG_CONFIG_DIRS=${CMAKE_CURRENT_BINARY_DIR}/test-config-dirs")
    endfunction()

    sendspin_add_test(app_test)
    sendspin_add_test(autostart_test)
    sendspin_add_test(audio/drift_corrector_test)
    sendspin_add_test(audio/outputs_test)
    sendspin_add_test(audio/pcm_ring_test)
    sendspin_add_test(audio/play_along_test)
    sendspin_add_test(audio/playback_stream_test)
    sendspin_add_test(audio/player_formats_test)
    sendspin_add_test(audio/sendspin_output_test)
    sendspin_add_test(audio/volume_curve_test)
    sendspin_add_test(blob_store_test)
    sendspin_add_test(cast/cast_engine_test)
    sendspin_add_test(cast/cast_feed_test)
    sendspin_add_test(cast/speaker_menu_test)
    sendspin_add_test(client/client_test)
    sendspin_add_test(client/mdns_advertiser_test)
    sendspin_add_test(client/source_feed_test)
    sendspin_add_test(device_name_test)
    sendspin_add_test(discord_ipc_test)
    sendspin_add_test(discord_presence_test)
    sendspin_add_test(logging_test)
    sendspin_add_test(mpris_test)
    sendspin_add_test(notifier_test)
    sendspin_add_test(paired_servers_test)
    sendspin_add_test(persistence_test)
    sendspin_add_test(picker/picker_test)
    sendspin_add_test(settings_model_test)
    sendspin_add_test(single_instance_test)
    sendspin_add_test(update_checker_test)
    sendspin_add_test(update_installer_test)

    # Not a test: scripts/render-screenshots.sh runs it
    add_executable(render-screenshots ${CMAKE_SOURCE_DIR}/scripts/render-screenshots.cpp)
    target_link_libraries(render-screenshots PRIVATE sendspin_app Qt6::Test)
endif()
