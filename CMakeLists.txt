cmake_minimum_required(VERSION 3.20)

project(MoonlightWebRTC LANGUAGES CXX)

find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(h264_annex_b_reader
    src/H264AnnexBReader.cpp
)

target_compile_features(h264_annex_b_reader PUBLIC cxx_std_20)
target_include_directories(h264_annex_b_reader PUBLIC src)

add_executable(moonlight_webrtc
    src/main.cpp
)

target_compile_features(moonlight_webrtc PRIVATE cxx_std_20)
target_link_libraries(moonlight_webrtc PRIVATE
    h264_annex_b_reader
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
endif()
