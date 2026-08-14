cmake_minimum_required(VERSION 3.20)

project(MoonlightWebRTC LANGUAGES C CXX)

find_package(CURL CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)

set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/moonlight-common-c EXCLUDE_FROM_ALL)

add_library(h264_annex_b_reader
    src/H264AnnexBReader.cpp
)

target_compile_features(h264_annex_b_reader PUBLIC cxx_std_20)
target_include_directories(h264_annex_b_reader PUBLIC src)

add_library(opus_file_reader
    src/OpusFileReader.cpp
)

target_compile_features(opus_file_reader PUBLIC cxx_std_20)
target_include_directories(opus_file_reader PUBLIC src)

add_library(webrtc_media_sender
    src/webrtc/WebRtcMediaSender.cpp
)

target_compile_features(webrtc_media_sender PUBLIC cxx_std_20)
target_include_directories(webrtc_media_sender PUBLIC src)
target_link_libraries(webrtc_media_sender PUBLIC LibDataChannel::LibDataChannel)

add_library(gateway_protocol
    src/gateway/GatewayProtocol.cpp
    src/session/StreamSettings.cpp
    src/webrtc/SamsungSdp.cpp
)

target_compile_features(gateway_protocol PUBLIC cxx_std_20)
target_include_directories(gateway_protocol PUBLIC src)
target_link_libraries(gateway_protocol PUBLIC nlohmann_json::nlohmann_json)

add_library(application_artwork_cache
    src/gateway/ApplicationArtworkCache.cpp
)

target_compile_features(application_artwork_cache PUBLIC cxx_std_20)
target_include_directories(application_artwork_cache PUBLIC src)

add_library(gateway_lifecycle
    src/gateway/GatewayLifecycle.cpp
)

target_compile_features(gateway_lifecycle PUBLIC cxx_std_20)
target_include_directories(gateway_lifecycle PUBLIC src)

add_library(service_ipc_protocol
    src/gateway/ServiceIpcProtocol.cpp
)

target_compile_features(service_ipc_protocol PUBLIC cxx_std_20)
target_include_directories(service_ipc_protocol PUBLIC src)
target_link_libraries(service_ipc_protocol PUBLIC nlohmann_json::nlohmann_json)

add_library(service_ipc_server
    src/gateway/ServiceIpcServer.cpp
)

target_compile_features(service_ipc_server PUBLIC cxx_std_20)
target_include_directories(service_ipc_server PUBLIC src)
target_link_libraries(service_ipc_server PUBLIC service_ipc_protocol advapi32)

add_library(moonlight_video_profile
    src/moonlight/MoonlightVideoProfile.cpp
)

target_compile_features(moonlight_video_profile PUBLIC cxx_std_20)
target_include_directories(moonlight_video_profile PUBLIC src)
target_link_libraries(moonlight_video_profile PUBLIC
    gateway_protocol
    moonlight-common-c
)

add_library(moonlight_media_bridge
    src/media/HevcSpsParser.cpp
    src/moonlight/MoonlightMediaBridge.cpp
)

target_compile_features(moonlight_media_bridge PUBLIC cxx_std_20)
target_include_directories(moonlight_media_bridge PUBLIC src)
target_link_libraries(moonlight_media_bridge PUBLIC
    gateway_protocol
    moonlight-common-c
    moonlight_video_profile
)

add_library(moonlight_input_bridge
    src/moonlight/input/MoonlightInputBridgeV2.cpp
)

target_compile_features(moonlight_input_bridge PUBLIC cxx_std_20)
target_include_directories(moonlight_input_bridge PUBLIC src)
target_link_libraries(moonlight_input_bridge PUBLIC
    moonlight-common-c
    nlohmann_json::nlohmann_json
)

add_library(moonlight_control
    src/moonlight/HdrMetadata.cpp
    src/moonlight/control/MoonlightIdentity.cpp
    src/moonlight/control/MoonlightPairing.cpp
    src/moonlight/control/MoonlightSession.cpp
    src/moonlight/control/SunshineHttpClient.cpp
    src/platform/WindowsDisplayState.cpp
)

target_compile_features(moonlight_control PUBLIC cxx_std_20)
target_include_directories(moonlight_control PUBLIC src)
target_link_libraries(moonlight_control PUBLIC
    gateway_protocol
    moonlight-common-c
    moonlight_media_bridge
    moonlight_video_profile
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    pugixml::pugixml
    advapi32
    shell32
    ole32
    user32
    ws2_32
)

add_executable(moonlight_webrtc
    src/main.cpp
    src/gateway/WindowsServiceHost.cpp
)

target_compile_features(moonlight_webrtc PRIVATE cxx_std_20)
target_link_libraries(moonlight_webrtc PRIVATE
    application_artwork_cache
    gateway_lifecycle
    gateway_protocol
    service_ipc_server
    h264_annex_b_reader
    moonlight_control
    moonlight_input_bridge
    moonlight_media_bridge
    opus_file_reader
    webrtc_media_sender
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    advapi32
)

add_executable(moonlight_webrtc_tray WIN32
    src/tray/ConfigurationWindow.cpp
    src/tray/ServiceIpcClient.cpp
    src/tray/StatusMonitor.cpp
    src/tray/TrayMain.cpp
)

target_compile_features(moonlight_webrtc_tray PRIVATE cxx_std_20)
target_link_libraries(moonlight_webrtc_tray PRIVATE
    service_ipc_protocol
    nlohmann_json::nlohmann_json
    dwmapi
    ole32
    shell32
)

set_target_properties(moonlight_webrtc_tray PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

set_target_properties(moonlight_webrtc PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

include(CTest)

if(BUILD_TESTING)
    add_executable(h264_annex_b_inspect
        tests/H264AnnexBReaderTest.cpp
    )
    target_link_libraries(h264_annex_b_inspect PRIVATE h264_annex_b_reader)

    add_test(
        NAME h264_annex_b_inspect
        COMMAND h264_annex_b_inspect
            "${CMAKE_CURRENT_SOURCE_DIR}/samples/test-720p60.h264"
    )

    add_executable(opus_file_inspect
        tests/OpusFileReaderTest.cpp
    )
    target_link_libraries(opus_file_inspect PRIVATE opus_file_reader)

    add_test(
        NAME opus_file_inspect
        COMMAND opus_file_inspect
            "${CMAKE_CURRENT_SOURCE_DIR}/samples/test-tone-48k-stereo.opus"
    )

    add_executable(moonlight_media_bridge_test
        tests/MoonlightMediaBridgeTest.cpp
    )
    target_link_libraries(moonlight_media_bridge_test PRIVATE moonlight_media_bridge)

    add_test(
        NAME moonlight_media_bridge_test
        COMMAND moonlight_media_bridge_test
    )

    add_executable(moonlight_control_test
        tests/MoonlightControlTest.cpp
    )
    target_link_libraries(moonlight_control_test PRIVATE moonlight_control)

    add_test(
        NAME moonlight_control_test
        COMMAND moonlight_control_test
    )

    add_executable(moonlight_input_bridge_test
        tests/MoonlightInputBridgeV2Test.cpp
    )
    target_link_libraries(moonlight_input_bridge_test PRIVATE moonlight_input_bridge)

    add_test(
        NAME moonlight_input_bridge_test
        COMMAND moonlight_input_bridge_test
    )

    add_executable(gateway_protocol_test
        tests/GatewayProtocolTest.cpp
    )
    target_link_libraries(gateway_protocol_test PRIVATE gateway_protocol)

    add_test(
        NAME gateway_protocol_test
        COMMAND gateway_protocol_test
    )

    add_executable(application_artwork_cache_test
        tests/ApplicationArtworkCacheTest.cpp
    )
    target_link_libraries(application_artwork_cache_test PRIVATE application_artwork_cache)

    add_test(
        NAME application_artwork_cache_test
        COMMAND application_artwork_cache_test
    )

    add_executable(webrtc_media_sender_test
        tests/WebRtcMediaSenderTest.cpp
    )
    target_link_libraries(webrtc_media_sender_test PRIVATE webrtc_media_sender)

    add_test(
        NAME webrtc_media_sender_test
        COMMAND webrtc_media_sender_test
    )

    add_executable(gateway_lifecycle_test
        tests/GatewayLifecycleTest.cpp
    )
    target_link_libraries(gateway_lifecycle_test PRIVATE gateway_lifecycle)

    add_test(
        NAME gateway_lifecycle_test
        COMMAND gateway_lifecycle_test
    )

    add_executable(service_ipc_protocol_test
        tests/ServiceIpcProtocolTest.cpp
    )
    target_link_libraries(service_ipc_protocol_test PRIVATE service_ipc_protocol)

    add_test(
        NAME service_ipc_protocol_test
        COMMAND service_ipc_protocol_test
    )
endif()
