# Everything but main(), so the tests link it
add_library(sendspin_app STATIC
    app.cpp
    audio/sendspin_output.cpp
    blob_store.cpp
    cast/cast_engine.cpp
    cast/cast_feed.cpp
    cast/speaker_menu.cpp
    cast/speaker_model.cpp
    client/client.cpp
    client/mdns_advertiser.cpp
    client/source_feed.cpp
    device_name.cpp
    headless.cpp
    notifier.cpp
    persistence.cpp
    single_instance.cpp
)
target_include_directories(sendspin_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(sendspin_app PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sendspin_app PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    sendspin_server_cpp
    sendspin
    PkgConfig::PIPEWIRE
    PkgConfig::AVAHI
)
# PUBLIC reaches the app and the tests, and nothing third-party
if(SENDSPIN_SANITIZE)
    target_compile_options(sendspin_app PUBLIC
        -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(sendspin_app PUBLIC -fsanitize=address,undefined)
endif()

add_executable(sendspin-for-linux main.cpp)
target_compile_options(sendspin-for-linux PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sendspin-for-linux PRIVATE sendspin_app)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)

    # SOURCE is the test's path without .cpp; the target takes its file name
    function(sendspin_add_test SOURCE)
        get_filename_component(NAME ${SOURCE} NAME)
        add_executable(${NAME} ${SOURCE}.cpp)
        target_compile_options(${NAME} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_libraries(${NAME} PRIVATE sendspin_app Qt6::Test)
        add_test(NAME ${NAME} COMMAND ${NAME})
        # No display in CI
        set_tests_properties(${NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()

    sendspin_add_test(app_test)
    sendspin_add_test(audio/sendspin_output_test)
    sendspin_add_test(blob_store_test)
    sendspin_add_test(cast/cast_engine_test)
    sendspin_add_test(cast/cast_feed_test)
    sendspin_add_test(cast/speaker_menu_test)
    sendspin_add_test(client/client_test)
    sendspin_add_test(client/mdns_advertiser_test)
    sendspin_add_test(client/source_feed_test)
    sendspin_add_test(device_name_test)
    sendspin_add_test(persistence_test)
    sendspin_add_test(single_instance_test)
endif()
