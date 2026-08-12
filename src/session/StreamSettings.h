#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace gateway {

enum class VideoCodec {
    H264,
};

struct StreamSettings {
    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrateKbps = 12000;
    VideoCodec codec = VideoCodec::H264;
    bool hdr = false;
    int audioChannels = 2;

    bool operator==(const StreamSettings&) const = default;
};

inline constexpr std::array SupportedResolutions{
    std::pair{1280, 720},
    std::pair{1920, 1080},
};

inline constexpr std::array SupportedBitratesKbps{
    10000,
    12000,
    15000,
    20000,
    25000,
    30000,
};

StreamSettings defaultStreamSettings(int width = 1280, int height = 720);
std::optional<std::string> validateStreamSettings(const StreamSettings& settings);
std::string_view videoCodecName(VideoCodec codec);
std::optional<VideoCodec> parseVideoCodec(std::string_view name);

} // namespace gateway
