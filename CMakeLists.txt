cmake_minimum_required(VERSION 3.20)

project(MoonlightWebRTC LANGUAGES CXX)

add_executable(moonlight_webrtc_toolchain
    src/main.cpp
)

target_compile_features(moonlight_webrtc_toolchain PRIVATE cxx_std_20)
set_target_properties(moonlight_webrtc_toolchain PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
