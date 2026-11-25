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
    headless.cpp
    mpris.cpp
    notifier.cpp
    paired_servers.cpp
    persistence.cpp
    picker/picker.cpp
    picker/picker_window.cpp
    settings_model.cpp
    single_instance.cpp
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
# PUBLIC reaches the app and the tests, and nothing third-party
if(SENDSPIN_SANITIZE)
    target_compile_options(sendspin_app PUBLIC
        -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(sendspin_app PUBLIC -fsanitize=address,undefined)
endif()

add_executable(sendspin-for-linux main.cpp)
target_compile_options(sendspin-for-linux PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sendspin-for-linux PRIVATE sendspin_app)

include(GNUInstallDirs)
install(TARGETS sendspin-for-linux)
# The launcher's entry, and the tray's icons where the panel finds and colours them
install(FILES ${CMAKE_SOURCE_DIR}/data/com.sendspin_audio.Sendspin.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${SENDSPIN_ICONS}/sendspin-symbolic.svg ${SENDSPIN_ICONS}/sendspin-waves-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps)
install(FILES ${SENDSPIN_ICONS}/sendspin-symbolic.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME com.sendspin_audio.Sendspin.svg)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)

    # SOURCE is the test's path without .cpp; the target takes its file name
    function(sendspin_add_test SOURCE)
        get_filename_component(NAME ${SOURCE} NAME)
        add_executable(${NAME} ${SOURCE}.cpp)
        target_compile_options(${NAME} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${NAME} PRIVATE sendspin_app Qt6::Test)
        add_test(NAME ${NAME} COMMAND ${NAME})
        # No display or GPU in CI, and never the settings of whoever runs the tests
        set_tests_properties(${NAME} PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-config")
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
    sendspin_add_test(mpris_test)
    sendspin_add_test(paired_servers_test)
    sendspin_add_test(persistence_test)
    sendspin_add_test(picker/picker_test)
    sendspin_add_test(settings_model_test)
    sendspin_add_test(single_instance_test)
endif()
