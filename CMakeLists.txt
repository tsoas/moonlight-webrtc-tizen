cmake_minimum_required(VERSION 3.20)

project(MoonlightWebRTC LANGUAGES CXX)

find_package(LibDataChannel CONFIG REQUIRED)

add_executable(moonlight_webrtc
    src/main.cpp
)

target_compile_features(moonlight_webrtc PRIVATE cxx_std_20)
target_link_libraries(moonlight_webrtc PRIVATE LibDataChannel::LibDataChannel)

set_target_properties(moonlight_webrtc PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
