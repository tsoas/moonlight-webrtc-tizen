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

add_library(moonlight_media_bridge
    src/moonlight/MoonlightMediaBridge.cpp
)

target_compile_features(moonlight_media_bridge PUBLIC cxx_std_20)
target_include_directories(moonlight_media_bridge PUBLIC src)
target_link_libraries(moonlight_media_bridge PUBLIC
    gateway_protocol
    moonlight-common-c
)

add_library(moonlight_input_bridge
    src/moonlight/input/MoonlightInputBridge.cpp
)

target_compile_features(moonlight_input_bridge PUBLIC cxx_std_20)
target_include_directories(moonlight_input_bridge PUBLIC src)
target_link_libraries(moonlight_input_bridge PUBLIC
    moonlight-common-c
    nlohmann_json::nlohmann_json
)

add_library(moonlight_control
    src/moonlight/control/MoonlightIdentity.cpp
    src/moonlight/control/MoonlightPairing.cpp
    src/moonlight/control/MoonlightSession.cpp
    src/moonlight/control/SunshineHttpClient.cpp
)

target_compile_features(moonlight_control PUBLIC cxx_std_20)
target_include_directories(moonlight_control PUBLIC src)
target_link_libraries(moonlight_control PUBLIC
    gateway_protocol
    moonlight-common-c
    moonlight_media_bridge
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    OpenSSL::SSL
    pugixml::pugixml
    ws2_32
)

add_executable(moonlight_webrtc
    src/main.cpp
)

target_compile_features(moonlight_webrtc PRIVATE cxx_std_20)
target_link_libraries(moonlight_webrtc PRIVATE
    gateway_protocol
    h264_annex_b_reader
    moonlight_control
    moonlight_input_bridge
    moonlight_media_bridge
    opus_file_reader
    webrtc_media_sender
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
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
        tests/MoonlightInputBridgeTest.cpp
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

    add_executable(webrtc_media_sender_test
        tests/WebRtcMediaSenderTest.cpp
    )
    target_link_libraries(webrtc_media_sender_test PRIVATE webrtc_media_sender)

    add_test(
        NAME webrtc_media_sender_test
        COMMAND webrtc_media_sender_test
    )
endif()
